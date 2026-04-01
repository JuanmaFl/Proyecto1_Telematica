"""
Auth Service — Puerto 9090
Servicio externo de identidad. Valida usuarios y retorna su rol.
El servidor web consulta este servicio antes de dar acceso.
"""

import socket
import json
import threading

HOST = "localhost"
PORT = 9090

# Base de usuarios — en producción sería una BD externa
USERS = {
    "admin":    {"password": "admin123",  "role": "OPERATOR"},
    "operator1": {"password": "op1pass",  "role": "OPERATOR"},
    "operator2": {"password": "op2pass",  "role": "OPERATOR"},
    "sensor1":  {"password": "sens1pass", "role": "SENSOR"},
    "sensor2":  {"password": "sens2pass", "role": "SENSOR"},
}


def validate_user(username, password):
    user = USERS.get(username)
    if user and user["password"] == password:
        return {"valid": True, "role": user["role"], "username": username}
    return {"valid": False, "role": None, "username": username}


def parse_query_params(path):
    params = {}
    if "?" in path:
        query = path.split("?", 1)[1]
        for pair in query.split("&"):
            if "=" in pair:
                k, v = pair.split("=", 1)
                params[k] = v
    return params


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