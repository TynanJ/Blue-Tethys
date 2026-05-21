/**
 * direct_ekf.c
 * Direct 2D extended Kalman filter for multi-anchor range localization.
 *
 * Matrices are flat double arrays in row-major order.
 * Fixed-size 4×4 helpers use stack allocation; variable-size helpers
 * (those depending on n_valid) use caller-supplied or heap buffers.
 */

#include "direct_ekf.h"

#include <math.h>    /* sqrt, isnan */
#include <string.h>  /* memcpy, memset */
#include <stdlib.h>  /* malloc, free */
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* =========================================================================
 * Compile-time constants
 * ====================================================================== */
#define STATE_DIM   4
#define SS          (STATE_DIM * STATE_DIM)   /* 16 – elements in a 4×4 mat */
#define RANGE_GUARD 1e-6

float rssi_to_distance(int8_t rssi, int8_t tx_power, float path_loss_exponent)
{
    /* Path loss model: PL(d) = PL(d0) + 10*n*log10(d/d0)
     * where d0 = 1m, PL(d0) = tx_power
     * 
     * Solving for distance d:
     * rssi = tx_power - 10*n*log10(d)
     * 10*n*log10(d) = tx_power - rssi
     * log10(d) = (tx_power - rssi) / (10*n)
     * d = 10^((tx_power - rssi) / (10*n))
     */
    
    if (rssi == 0) {
        return 0.0f;  /* Invalid RSSI */
    }
    
    float numerator = (float)(tx_power - rssi);
    float denominator = 10.0f * path_loss_exponent;
    float exponent = numerator / denominator;
    
    float distance = powf(10.0f, exponent);
    
    /* Clamp to reasonable bounds */
    if (distance < 0.1f) {
        distance = 0.1f;
    }
    if (distance > 1000.0f) {
        distance = 1000.0f;
    }
    
    return distance;
}

/* =========================================================================
 * Tiny fixed-size (4×4) matrix helpers  –  all operate on double[16]
 * ====================================================================== */

/** dst = 0 */
static void mat4_zero(double *dst)
{
    memset(dst, 0, SS * sizeof(double));
}

/** dst = identity */
static void mat4_eye(double *dst)
{
    mat4_zero(dst);
    for (int i = 0; i < STATE_DIM; ++i) dst[i * STATE_DIM + i] = 1.0;
}

/** dst = a + b  (4×4) */
static void mat4_add(const double *a, const double *b, double *dst)
{
    for (int i = 0; i < SS; ++i) dst[i] = a[i] + b[i];
}

/** dst = a * b  (4×4) */
static void mat4_mul(const double *a, const double *b, double *dst)
{
    double tmp[SS];
    for (int r = 0; r < STATE_DIM; ++r)
        for (int c = 0; c < STATE_DIM; ++c) {
            double s = 0.0;
            for (int k = 0; k < STATE_DIM; ++k)
                s += a[r * STATE_DIM + k] * b[k * STATE_DIM + c];
            tmp[r * STATE_DIM + c] = s;
        }
    memcpy(dst, tmp, SS * sizeof(double));
}

/** dst = a^T  (4×4) */
static void mat4_transpose(const double *a, double *dst)
{
    for (int r = 0; r < STATE_DIM; ++r)
        for (int c = 0; c < STATE_DIM; ++c)
            dst[c * STATE_DIM + r] = a[r * STATE_DIM + c];
}

/** dst = a * b * a^T  (4×4) */
static void mat4_sandwich(const double *a, const double *b, double *dst)
{
    double tmp[SS];
    mat4_mul(a, b, tmp);   /* tmp = a*b          */
    double at[SS];
    mat4_transpose(a, at); /* at  = a^T           */
    mat4_mul(tmp, at, dst);/* dst = (a*b) * a^T   */
}

/** dst = s * I  (4×4, scalar * identity) */
static void mat4_scale_eye(double s, double *dst)
{
    mat4_zero(dst);
    for (int i = 0; i < STATE_DIM; ++i) dst[i * STATE_DIM + i] = s;
}

/* =========================================================================
 * Variable-size (m×n) matrix helpers
 * All shapes are passed explicitly; arrays are caller-allocated.
 * ====================================================================== */

/**
 * C[r,c] += A[r,k] * B[k,c]
 * dst (m×p) = a (m×n) * b (n×p)   –– dst must be zeroed by caller
 */
static void matmul(const double *a, size_t m, size_t n,
                   const double *b,            size_t p,
                   double       *dst)
{
    for (size_t r = 0; r < m; ++r)
        for (size_t c = 0; c < p; ++c) {
            double s = 0.0;
            for (size_t k = 0; k < n; ++k)
                s += a[r * n + k] * b[k * p + c];
            dst[r * p + c] = s;
        }
}

/** dst (n×m) = a (m×n) ^T */
static void mattranspose(const double *a, size_t m, size_t n, double *dst)
{
    for (size_t r = 0; r < m; ++r)
        for (size_t c = 0; c < n; ++c)
            dst[c * m + r] = a[r * n + c];
}

/* =========================================================================
 * Pseudoinverse via explicit Gaussian elimination on the innovation
 * covariance S (m×m).  For small m (typical: 2–8 anchors) this is fine.
 * We compute S^{-1} using Gauss-Jordan; if S is singular we fall back to
 * the transpose (acts like a small-norm right-inverse).
 * ====================================================================== */

/**
 * Compute dst = pinv(S) where S is (m×m), using Gauss-Jordan with partial
 * pivoting.  dst must be an (m×m) caller-allocated buffer.
 * Returns 1 on success, 0 if S is numerically singular (dst set to S^T).
 */
static int mat_pinv(const double *S, size_t m, double *dst)
{
    /* Augmented matrix [S | I] stored in a single (m × 2m) array. */
    double *aug = (double *)malloc(m * 2 * m * sizeof(double));
    if (!aug) return 0;

    /* Initialise augmented matrix. */
    for (size_t r = 0; r < m; ++r) {
        for (size_t c = 0; c < m; ++c)
            aug[r * 2 * m + c] = S[r * m + c];
        for (size_t c = m; c < 2 * m; ++c)
            aug[r * 2 * m + c] = (c - m == r) ? 1.0 : 0.0;
    }

    /* Forward elimination with partial pivoting. */
    for (size_t col = 0; col < m; ++col) {
        /* Find pivot row. */
        size_t pivot = col;
        double best  = fabs(aug[col * 2 * m + col]);
        for (size_t r = col + 1; r < m; ++r) {
            double v = fabs(aug[r * 2 * m + col]);
            if (v > best) { best = v; pivot = r; }
        }
        if (best < 1e-14) {
            /* Singular – fall back to transpose */
            free(aug);
            mattranspose(S, m, m, dst);
            return 0;
        }
        /* Swap rows col <-> pivot. */
        if (pivot != col) {
            for (size_t c = 0; c < 2 * m; ++c) {
                double tmp = aug[col * 2 * m + c];
                aug[col   * 2 * m + c] = aug[pivot * 2 * m + c];
                aug[pivot * 2 * m + c] = tmp;
            }
        }
        /* Scale pivot row. */
        double inv = 1.0 / aug[col * 2 * m + col];
        for (size_t c = 0; c < 2 * m; ++c)
            aug[col * 2 * m + c] *= inv;
        /* Eliminate column. */
        for (size_t r = 0; r < m; ++r) {
            if (r == col) continue;
            double factor = aug[r * 2 * m + col];
            for (size_t c = 0; c < 2 * m; ++c)
                aug[r * 2 * m + c] -= factor * aug[col * 2 * m + c];
        }
    }

    /* Extract right half into dst. */
    for (size_t r = 0; r < m; ++r)
        for (size_t c = 0; c < m; ++c)
            dst[r * m + c] = aug[r * 2 * m + (m + c)];

    free(aug);
    return 1;
}

/* =========================================================================
 * EKF building blocks
 * ====================================================================== */

/**
 * Build the constant-velocity transition matrix F (4×4, row-major).
 *
 *  [ 1  0  dt  0 ]
 *  [ 0  1   0 dt ]
 *  [ 0  0   1  0 ]
 *  [ 0  0   0  1 ]
 */
static void constant_velocity_transition(double dt, double *F)
{
    mat4_eye(F);
    F[0 * STATE_DIM + 2] = dt;   /* x  += vx*dt */
    F[1 * STATE_DIM + 3] = dt;   /* y  += vy*dt */
}

/**
 * Build the 4×4 process-noise matrix Q (row-major).
 *
 * Q_1d = q_var * [[dt^4/4, dt^3/2],
 *                 [dt^3/2, dt^2  ]]
 *
 * Q is the block-interleaved version for state [x, y, vx, vy]:
 *   rows/cols ordered x, y, vx, vy
 */
static void process_noise(double dt, double q_var, double *Q)
{
    double dt2 = dt  * dt;
    double dt3 = dt2 * dt;
    double dt4 = dt2 * dt2;

    double q00 = q_var * dt4 / 4.0;  /* position-position    */
    double q01 = q_var * dt3 / 2.0;  /* position-velocity    */
    double q11 = q_var * dt2;         /* velocity-velocity    */

    mat4_zero(Q);
    /* x-x       */ Q[0 * 4 + 0] = q00;
    /* x-vx      */ Q[0 * 4 + 2] = q01;
    /* y-y       */ Q[1 * 4 + 1] = q00;
    /* y-vy      */ Q[1 * 4 + 3] = q01;
    /* vx-x      */ Q[2 * 4 + 0] = q01;
    /* vx-vx     */ Q[2 * 4 + 2] = q11;
    /* vy-y      */ Q[3 * 4 + 1] = q01;
    /* vy-vy     */ Q[3 * 4 + 3] = q11;
}

/**
 * h(x) – predicted ranges to each anchor.
 *
 * @param state     State vector [x, y, vx, vy].
 * @param anchors   (n_anchors × 2) row-major array.
 * @param n_anchors Number of anchors.
 * @param h_out     Output array of length n_anchors.
 */
static void measurement_function(const double *state,
                                  const double *anchors,
                                  size_t        n_anchors,
                                  double       *h_out)
{
    double px = state[0], py = state[1];
    for (size_t i = 0; i < n_anchors; ++i) {
        double dx = px - anchors[i * 2 + 0];
        double dy = py - anchors[i * 2 + 1];
        h_out[i]  = sqrt(dx * dx + dy * dy);
    }
}

/**
 * H = dh/dx – Jacobian of measurement function (n_anchors × 4).
 *
 * @param state     State vector [x, y, vx, vy].
 * @param anchors   (n_anchors × 2) row-major array.
 * @param n_anchors Number of anchors.
 * @param H_out     Output (n_anchors × 4) row-major array, zeroed by caller.
 */
static void measurement_jacobian(const double *state,
                                  const double *anchors,
                                  size_t        n_anchors,
                                  double       *H_out)
{
    double px = state[0], py = state[1];
    for (size_t i = 0; i < n_anchors; ++i) {
        double dx    = px - anchors[i * 2 + 0];
        double dy    = py - anchors[i * 2 + 1];
        double range = sqrt(dx * dx + dy * dy);
        double safe  = range < RANGE_GUARD ? RANGE_GUARD : range;
        H_out[i * STATE_DIM + 0] = dx / safe;   /* dh/dx  */
        H_out[i * STATE_DIM + 1] = dy / safe;   /* dh/dy  */
        /* H[:,2] and H[:,3] are 0 (velocity has no direct effect on range) */
    }
}

/* =========================================================================
 * Public API
 * Instead of explicitly solving least squares at each timestep, we use an 
 * EKF which generalises least squares to a recursive nonlinear framework.
 * ====================================================================== */

void run_direct_ekf(
    const double    *z,
    const double    *anchors,
    size_t           n_steps,
    size_t           n_anchors,
    double           dt,
    double           q_var,
    double           r_var,
    double           p0,
    const double    *x0,
    DirectEKFResult *result)
{
    /* ------------------------------------------------------------------ */
    /* Persistent filter state                                             */
    /* ------------------------------------------------------------------ */
    double x[STATE_DIM];
    if (x0) {
        memcpy(x, x0, STATE_DIM * sizeof(double));
    } else {
        memset(x, 0, sizeof(x));
    }

    double P[SS];
    mat4_scale_eye(p0, P);

    /* Pre-build constant matrices. */
    double F[SS], Q[SS], I4[SS];
    constant_velocity_transition(dt, F);
    process_noise(dt, q_var, Q);
    mat4_eye(I4);

    double Ft[SS];
    mat4_transpose(F, Ft);

    /* ------------------------------------------------------------------ */
    /* Initialise output buffers to NaN                                    */
    /* ------------------------------------------------------------------ */
    for (size_t i = 0; i < n_steps * STATE_DIM; ++i)
        result->states[i] = (double)NAN;
    for (size_t i = 0; i < n_steps * SS; ++i)
        result->covariances[i] = (double)NAN;

    /* ------------------------------------------------------------------ */
    /* Scratch allocations (worst-case size = n_anchors)                   */
    /* ------------------------------------------------------------------ */
    size_t m_max = n_anchors;
    double *valid_anchors = (double *)malloc(m_max * 2  * sizeof(double));
    double *z_k           = (double *)malloc(m_max      * sizeof(double));
    double *h_k           = (double *)malloc(m_max      * sizeof(double));
    double *H_k           = (double *)malloc(m_max * STATE_DIM * sizeof(double));
    double *Ht_k          = (double *)malloc(STATE_DIM * m_max * sizeof(double));
    double *S_k           = (double *)malloc(m_max * m_max * sizeof(double));
    double *S_inv         = (double *)malloc(m_max * m_max * sizeof(double));
    double *K_k           = (double *)malloc(STATE_DIM * m_max * sizeof(double));
    double *tmp_4m        = (double *)malloc(STATE_DIM * m_max * sizeof(double));
    double *tmp_mm        = (double *)malloc(m_max * m_max * sizeof(double));
    double *y_k           = (double *)malloc(m_max      * sizeof(double));

    if (!valid_anchors || !z_k || !h_k || !H_k || !Ht_k ||
        !S_k || !S_inv || !K_k || !tmp_4m || !tmp_mm || !y_k)
        goto cleanup;

    /* ------------------------------------------------------------------ */
    /* Main filter loop                                                    */
    /* ------------------------------------------------------------------ */
    for (size_t step = 0; step < n_steps; ++step) {

        /* ---- Predict ---- */
        /* x = F @ x */
        {
            double xp[STATE_DIM] = {0};
            for (int r = 0; r < STATE_DIM; ++r)
                for (int c = 0; c < STATE_DIM; ++c)
                    xp[r] += F[r * STATE_DIM + c] * x[c];
            memcpy(x, xp, sizeof(x));
        }

        /* P = F @ P @ F^T + Q */
        {
            double tmp[SS];
            mat4_sandwich(F, P, tmp);   /* tmp = F P F^T */
            mat4_add(tmp, Q, P);
        }

        /* ---- Gather valid measurements for this step ---- */
        size_t n_valid = 0;
        const double *z_row = z + step * n_anchors;
        for (size_t a = 0; a < n_anchors; ++a) {
            if (!isnan(z_row[a])) {
                valid_anchors[n_valid * 2 + 0] = anchors[a * 2 + 0];
                valid_anchors[n_valid * 2 + 1] = anchors[a * 2 + 1];
                z_k[n_valid] = z_row[a];
                ++n_valid;
            }
        }

        /* ---- Update (only if we have at least one measurement) ---- */
        if (n_valid > 0) {
            size_t m = n_valid;   /* shorthand */

            /* h_k = h(x, anchors_k)   [m] */
            measurement_function(x, valid_anchors, m, h_k);

            /* H_k = jacobian           [m × 4] */
            memset(H_k, 0, m * STATE_DIM * sizeof(double));
            measurement_jacobian(x, valid_anchors, m, H_k);

            /* Ht_k = H_k^T             [4 × m] */
            mattranspose(H_k, m, STATE_DIM, Ht_k);

            /* S_k = H_k @ P @ H_k^T + R_k
             *     = H_k @ (P @ H_k^T) + r_var * I_m
             *
             *  tmp_4m [4×m] = P @ H_k^T
             *  S_k    [m×m] = H_k @ tmp_4m  +  r_var * I_m
             */
            memset(tmp_4m, 0, STATE_DIM * m * sizeof(double));
            matmul(P, STATE_DIM, STATE_DIM, Ht_k, m, tmp_4m);

            memset(S_k, 0, m * m * sizeof(double));
            matmul(H_k, m, STATE_DIM, tmp_4m, m, S_k);   /* H P H^T */
            for (size_t i = 0; i < m; ++i)
                S_k[i * m + i] += r_var;                  /* + R */

            /* S_inv = pinv(S_k)  [m×m] */
            mat_pinv(S_k, m, S_inv);

            /* K_k = P @ H_k^T @ S_inv  [4×m]
             *
             *  tmp_4m already holds P @ H_k^T   [4×m]
             *  K_k [4×m] = tmp_4m @ S_inv
             */
            memset(K_k, 0, STATE_DIM * m * sizeof(double));
            matmul(tmp_4m, STATE_DIM, m, S_inv, m, K_k);

            /* y_k = z_k - h_k  (innovation) */
            for (size_t i = 0; i < m; ++i)
                y_k[i] = z_k[i] - h_k[i];

            /* x = x + K_k @ y_k */
            for (int r = 0; r < STATE_DIM; ++r) {
                double s = 0.0;
                for (size_t i = 0; i < m; ++i)
                    s += K_k[r * m + i] * y_k[i];
                x[r] += s;
            }

            /*
             * Joseph-form covariance update (numerically stable):
             *   IKH = I - K_k @ H_k                    [4×4]
             *   P   = IKH @ P @ IKH^T + K_k @ R @ K_k^T
             */

            /* IKH = I - K_k @ H_k  [4×4] */
            double KH[SS], IKH[SS];
            memset(KH, 0, sizeof(KH));
            matmul(K_k, STATE_DIM, m, H_k, STATE_DIM, KH);
            for (int i = 0; i < SS; ++i) IKH[i] = I4[i] - KH[i];

            /* term1 = IKH @ P @ IKH^T  [4×4] */
            double term1[SS];
            mat4_sandwich(IKH, P, term1);

            /* term2 = K_k @ R @ K_k^T
             *       = r_var * (K_k @ K_k^T)           [4×4]
             *
             *  Kt_k [m×4] = K_k^T
             *  KKt  [4×4] = K_k @ Kt_k
             */
            double Kt_k[STATE_DIM * m_max];          /* stack; m ≤ m_max */
            mattranspose(K_k, STATE_DIM, m, Kt_k);
            double KKt[SS];
            memset(KKt, 0, sizeof(KKt));
            matmul(K_k, STATE_DIM, m, Kt_k, STATE_DIM, KKt);
            double term2[SS];
            for (int i = 0; i < SS; ++i) term2[i] = r_var * KKt[i];

            /* P = term1 + term2 */
            mat4_add(term1, term2, P);
        }

        /* ---- Store results ---- */
        memcpy(result->states      + step * STATE_DIM, x, STATE_DIM * sizeof(double));
        memcpy(result->covariances + step * SS,        P, SS        * sizeof(double));
    }

cleanup:
    free(valid_anchors);
    free(z_k);
    free(h_k);
    free(H_k);
    free(Ht_k);
    free(S_k);
    free(S_inv);
    free(K_k);
    free(tmp_4m);
    free(tmp_mm);
    free(y_k);
}
