from time import time

class MobilePosition:
    def __init__(self, init_x, init_y, init_z, init_time):
        self._x = init_x
        self._y = init_y
        self._z = init_z
        self._init_time = init_time

        self._velocity = 0
        self._total_distance_traveled = 0

    def update_position(self, new_x, new_y, new_z, measurement_time):
        # Update distance traveled (just base on 2D plane for now)
        self._total_distance_traveled += ((self._x - new_x) ** 2 + (self._y - new_y) ** 2) ** 0.5

        self._x = new_x
        self._y = new_y
        self._z = new_z

        # Update average velocity
        if measurement_time > self._init_time:
            self._velocity = self._total_distance_traveled / (measurement_time - self._init_time)

mobile_station_position = MobilePosition(0, 0, 0, time())
