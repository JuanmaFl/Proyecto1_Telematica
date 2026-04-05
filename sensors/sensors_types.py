import random # Para generar valores aleatorios de sensores
from sensor import Sensor # aqui se importa la clase base Sensor para crear nuestras clases específicas de sensores


class TemperatureSensor(Sensor):
    def __init__(self, sensor_id):
        super().__init__(sensor_id, "TEMPERATURE")

    def generate_value(self):
        return random.uniform(15.0, 95.0)  # Supera 80 ocasionalmente


class VibrationSensor(Sensor):
    def __init__(self, sensor_id):
        super().__init__(sensor_id, "VIBRATION")

    def generate_value(self):
        return random.uniform(0.0, 12.0)  # Supera 10 ocasionalmente


class EnergySensor(Sensor):
    def __init__(self, sensor_id):
        super().__init__(sensor_id, "ENERGY")

    def generate_value(self):
        return random.uniform(100.0, 600.0)  # Supera 500 ocasionalmente
