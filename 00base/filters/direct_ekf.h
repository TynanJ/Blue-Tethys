/**
 * direct_ekf.h
 * Direct 2D extended Kalman filter for multi-anchor range localization.
 *
 * State:       [x, y, vx, vy]  (length 4)
 * Measurement: ranges to a set of fixed anchors
 *
 * All matrices are stored in row-major order as flat double arrays.
 * A 4×4 matrix M has element M[r][c] at M[r*4 + c].
 */

#ifndef DIRECT_EKF_H
#define DIRECT_EKF_H

#include <stddef.h>

/**
 * Output buffers filled by run_direct_ekf().
 * Both are caller-allocated; see run_direct_ekf() for required sizes.
 */
typedef struct {
    double *states;       /**< Shape (n_steps, 4),    row-major. */
    double *covariances;  /**< Shape (n_steps, 4, 4), row-major. */
} DirectEKFResult;

/**
 * Run a direct EKF using multi-anchor range measurements.
 *
 * @param z           Range measurements, shape (n_steps × n_anchors), row-major.
 *                    NaN entries indicate dropped packets for that anchor/step.
 * @param anchors     Anchor coordinates, shape (n_anchors × 2), row-major.
 * @param n_steps     Number of time steps.
 * @param n_anchors   Number of anchors.
 * @param dt          Time step (seconds).
 * @param q_var       Process-noise variance scalar.
 * @param r_var       Measurement-noise variance scalar.
 * @param p0          Initial covariance diagonal value.
 * @param x0          Initial state [x, y, vx, vy], or NULL for zeros.
 * @param result      Output struct with caller-allocated buffers:
 *                      result->states      must hold n_steps * 4       doubles.
 *                      result->covariances must hold n_steps * 4 * 4   doubles.
 */
void run_direct_ekf(
    const double   *z,
    const double   *anchors,
    size_t          n_steps,
    size_t          n_anchors,
    double          dt,
    double          q_var,
    double          r_var,
    double          p0,
    const double   *x0,
    DirectEKFResult *result
);

#endif /* DIRECT_EKF_H */
