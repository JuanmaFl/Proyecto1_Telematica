[Main README](README.md) | [Protocolos](.github/PROTOCOLOS.md) 

## Prerrequisitos
- Docker Desktop instalado y corriendo
- Python 3 instalado
- Java JDK instalado

---

## Paso 1 — Clonar el repo

git clone https://github.com/JuanmaFl/Proyecto1_Telematica.git
cd Proyecto1_Telematica


## Paso 2 — Construir y correr el servidor en Docker

docker build -t iot-server .
docker run -p 8080:8080 -p 8081:8081 iot-server

Dejar esta ventana abierta.

## Paso 3 — Correr los sensores (nueva ventana CMD)

cd sensors
python main.py

Dejar esta ventana abierta.

## Paso 4 — Correr el Auth Service (nueva ventana CMD)

cd auth-service
python auth_service.py

Dejar esta ventana abierta.

## Paso 5 — Correr el Web Gateway (nueva ventana CMD)

cd auth-service
python web_gateway.py

Dejar esta ventana abierta.

## Paso 6 — Correr el cliente operador Java (nueva ventana CMD)

cd operator-client
javac ServerConnection.java OperatorGUI.java ChartPanel.java Main.java
java Main


## Paso 7 — Abrir la interfaz web
Abrir el browser en:

http://localhost:8082

Usuarios disponibles: admin / operator1 / operator2 — contraseñas: admin123 / op1pass / op2pass

---
