import threading
from sensors_types import TemperatureSensor, VibrationSensor, EnergySensor

# Aqui se establecen las simulaciones de los 5 sensores.
sensores = [
    TemperatureSensor("temp_01"),
    TemperatureSensor("temp_02"),
    VibrationSensor("vib_01"),
    VibrationSensor("vib_02"),
    EnergySensor("energy_01"),
]

# Lanzamos cada sensor en su propio hilo.
hilos = []
for sensor in sensores:
    t = threading.Thread(target=sensor.run, daemon=True)
    t.start()
    hilos.append(t)

# Esperamos a que los hilos terminen (en este caso, correrán indefinidamente hasta que se interrumpa el programa).
print("Todos los sensores iniciados. Presioná Ctrl+C para detener.")

try:
    for t in hilos:
        t.join()
except KeyboardInterrupt:
    print("\nSensores detenidos.")
