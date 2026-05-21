import serial
from time import sleep

from constants import *

class SerialInterface:
    def __init__(self, port : str):
        self._serial_port = serial.Serial(port, baudrate=9600, timeout=0.5)
        pass

    def serial_read_to_string(self) -> str:
        """
        Read response bytes from a command
        """
        
        response = ""
        while (self._serial_port.in_waiting > 0):
            response += self._serial_port.read().decode('utf-8')
        
        return response
    

    def _serial_send(self, message: bytes) -> None:
        """
        Send a byte-encoded serial message, and flush the buffer
        """
        self._serial_port.write(message)
        self._serial_port.flush()
        sleep(0.3)


basestation_serial_interface = SerialInterface(BASESTATION_SERIAL_PORT)
