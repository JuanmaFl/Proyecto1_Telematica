"""
Servicio de autenticacion externa — En el Puerto 9090
En este se validan los usuarios y sus roles (OPERATOR o SENSOR).
En el servidor web se consulta este servicio antes otorgar acceso a las funcionalidades.
"""

import socket
import json
import threading

HOST = "localhost"
PORT = 9090

# Base de datoss de usuarios con contraseñas y roles definidos
USERS = {
    "admin":    {"password": "admin123",  "role": "OPERATOR"},
    "operator1": {"password": "op1pass",  "role": "OPERATOR"},
    "operator2": {"password": "op2pass",  "role": "OPERATOR"},
    "sensor1":  {"password": "sens1pass", "role": "SENSOR"},
    "sensor2":  {"password": "sens2pass", "role": "SENSOR"},
}

# Esta funcion Valida el usuario y la Contraseña 
def validate_user(username, password):
    user = USERS.get(username)
    if user and user["password"] == password:
        return {"valid": True, "role": user["role"], "username": username}
    return {"valid": False, "role": None, "username": username}

# Esta funcion Parsea los parametros de la consulta GET
def parse_query_params(path):
    params = {}
    if "?" in path:
        query = path.split("?", 1)[1]
        for pair in query.split("&"):
            if "=" in pair:
                k, v = pair.split("=", 1)
                params[k] = v
    return params

# Aqui se manejan las solicitudes entrantes y se responde con un JSON indicando si es valido o no el usuario
def handle_request(conn):
    try:
        data = conn.recv(4096).decode()
        if not data:
            return

        lines = data.split("\r\n")
        first_line = lines[0].split(" ")
        if len(first_line) < 2:
            return

        method = first_line[0]
        path   = first_line[1]

        if method == "GET" and path.startswith("/validate"):
            params   = parse_query_params(path)
            username = params.get("user", "")
            password = params.get("pass", "")
            result   = validate_user(username, password)
            body     = json.dumps(result)

            response = (
                f"HTTP/1.1 200 OK\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"Connection: close\r\n"
                f"\r\n"
                f"{body}"
            )
            conn.sendall(response.encode())
            print(f"[AUTH] {username} → valid={result['valid']} role={result['role']}")
        else:
            body = '{"error":"not found"}'
            response = (
                f"HTTP/1.1 404 Not Found\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"Connection: close\r\n"
                f"\r\n"
                f"{body}"
            )
            conn.sendall(response.encode())
    except Exception as e:
        print(f"[AUTH] Error: {e}")
    finally:
        conn.close()

#Este es el amin que se queda escuchado en el puerto 9090 y cada vez que llega una solicitud se crea un nuevo hilo para manejarla
def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(10)
    print(f"[AUTH] Auth Service corriendo en puerto {PORT}")

    while True:
        conn, _ = server.accept()
        threading.Thread(target=handle_request, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()