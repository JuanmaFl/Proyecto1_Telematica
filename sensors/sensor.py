import socket   # Para la comunicación con el servidor
import time # Para manejar los intervalos de envío
import random # Para generar valores aleatorios de sensores
from config import SERVER_HOST, SERVER_PORT, SEND_INTERVAL # Configuración del servidor y frecuencia de envío


# Esta es la clase madre de todos los sensores cada sensor coge de esta clase y sobreescribe el metodo generate_value para generar su valor específico. 
class Sensor:
    def __init__(self, sensor_id, sensor_type):
        self.sensor_id = sensor_id
        self.sensor_type = sensor_type
        self.sock = None

    def connect(self):
        while True:
            try:
                # Resolución por nombre, nunca por IP
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.connect((SERVER_HOST, SERVER_PORT))
                print(f"[{self.sensor_id}] Conectado al servidor")
                self._register()
                return
            # Si hay un error de conexión, se muestra el error y se reintenta después de 5 segundos.
            except socket.error as e:
                print(f"[{self.sensor_id}] Error de conexión: {e}. Reintentando en 5s...")
                time.sleep(5)

    # Este método se encarga de registrar el sensor en el servidor enviando un mensaje con su ID y tipo.
    def _register(self):
        msg = f"REGISTER SENSOR {self.sensor_id} {self.sensor_type}\n"
        self.sock.sendall(msg.encode())
        response = self.sock.recv(1024).decode().strip()
        print(f"[{self.sensor_id}] Registro: {response}")

    def generate_value(self):
        # Cada subclase sobreescribe este método
        raise NotImplementedError

    # Este método se encarga de enviar la medición al servidor en el formato especificado y manejar la respuesta.
    def send_measurement(self):
        value = self.generate_value()
        timestamp = int(time.time())
        msg = f"DATA {self.sensor_id} {value:.2f} {timestamp}\n"
        try:
            # Enviamos el mensaje al servidor y esperamos una respuesta.
            self.sock.sendall(msg.encode())
            response = self.sock.recv(1024).decode().strip()
            print(f"[{self.sensor_id}] Enviado {value:.2f} → {response}")
        except socket.error as e:
            print(f"[{self.sensor_id}] Error al enviar: {e}")
            self.connect()  # Reconectar automáticamente
    # Aqui se estable la conxion con el servidor y entramos en un bucle infinito.
    def run(self):
        self.connect()
        while True:
            self.send_measurement()
            time.sleep(SEND_INTERVAL)
