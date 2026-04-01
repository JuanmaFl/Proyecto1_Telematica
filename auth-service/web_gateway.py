"""
Web Gateway — Puerto 8082
Sirve la interfaz web, valida credenciales contra el Auth Service (9090)
y consulta el estado del servidor IoT (8081).
"""

import socket
import json
import threading
from urllib.parse import parse_qs

AUTH_HOST    = "localhost"
AUTH_PORT    = 9090
IOT_HOST     = "localhost"
IOT_PORT     = 8081
GATEWAY_PORT = 8082
HOST         = "localhost"

DASHBOARD_CSS = """
<style>
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0}
  .header{background:#16213e;padding:16px 32px;display:flex;
          justify-content:space-between;align-items:center;
          border-bottom:2px solid #00ff99}
  .header h1{color:#00ff99;margin:0;font-size:20px}
  .header span{color:#aaa;font-size:13px}
  .container{padding:24px 32px}
  .stats{display:flex;gap:16px;margin-bottom:24px}
  .stat{background:#16213e;border-radius:10px;padding:20px;
        text-align:center;flex:1;border:1px solid #333}
  .stat span{display:block;font-size:32px;font-weight:bold;color:#00ff99}
  table{width:100%;border-collapse:collapse;background:#16213e;
        border-radius:10px;overflow:hidden}
  th{background:#0f3460;padding:12px;text-align:left;color:#00ff99;font-size:13px}
  td{padding:12px;font-size:13px;border-bottom:1px solid #222}
  .logout{color:#ff6464;text-decoration:none;font-size:13px}
  .note{color:#555;font-size:11px;margin-top:8px}
</style>
"""

LOGIN_CSS = """
<style>
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;
       display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
  .card{background:#16213e;padding:40px;border-radius:12px;
        box-shadow:0 8px 32px rgba(0,0,0,.4);width:360px}
  h2{text-align:center;color:#00ff99;margin-bottom:24px}
  label{display:block;margin:10px 0 4px;font-size:14px;color:#aaa}
  input{width:100%;padding:10px;border:1px solid #333;border-radius:6px;
        background:#0f3460;color:#eee;box-sizing:border-box;font-size:14px}
  button{width:100%;padding:12px;margin-top:20px;background:#00ff99;
         color:#1a1a2e;border:none;border-radius:6px;font-size:16px;
         font-weight:bold;cursor:pointer}
  button:hover{background:#00dda0}
  .error{color:#ff6464;text-align:center;margin-top:12px;font-size:14px}
  .note{color:#666;text-align:center;margin-top:16px;font-size:12px}
</style>
"""

def get_login_html(error=""):
    return f"""<!DOCTYPE html>
<html><head><meta charset='UTF-8'>
<title>IoT Monitoring — Login</title>
{LOGIN_CSS}
</head><body>
<div class='card'>
  <h2>⬡ IoT Monitoring</h2>
  <form method='POST' action='/login'>
    <label>Usuario</label>
    <input name='user' placeholder='admin' required>
    <label>Password</label>
    <input name='pass' type='password' placeholder='••••••••' required>
    <button type='submit'>Ingresar</button>
  </form>
  {error}
  <p class='note'>Usuarios: admin / operator1 / operator2</p>
</div></body></html>"""


def http_get(host, port, path):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect((host, port))
        req = f"GET {path} HTTP/1.0\r\nHost: {host}\r\n\r\n"
        s.sendall(req.encode())
        response = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            response += chunk
        s.close()
        if b"\r\n\r\n" in response:
            return response.split(b"\r\n\r\n", 1)[1].decode()
        return ""
    except Exception as e:
        print(f"[GATEWAY] Error consultando {host}:{port}{path} → {e}")
        return None


def send_response(conn, status_code, status_text, content_type, body):
    body_bytes = body.encode()
    response = (
        f"HTTP/1.1 {status_code} {status_text}\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body_bytes)}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    ).encode() + body_bytes
    conn.sendall(response)

def get_sensors_tcp():
    """Consulta la lista de sensores via TCP al servidor IoT."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect((IOT_HOST, 8080))
        s.sendall(b"REGISTER OPERATOR webgateway\n")
        s.recv(1024)
        s.sendall(b"LIST SENSORS\n")
        data = s.recv(4096).decode()
        s.close()
        return data
    except Exception as e:
        print(f"[GATEWAY] Error consultando sensores TCP: {e}")
        return None
    
def build_dashboard(username, role, iot_status, iot_sensors):
    sensors_rows = ""
    if iot_sensors:
        parts = iot_sensors.replace("SENSORS", "").strip().split(" ")
        for part in parts:
            if not part or part == "(none)":
                continue
            fields = part.split(":")
            if len(fields) >= 3:
                color = "#1e3a1e" if fields[2] == "ACTIVE" else "#3a1e1e"
                dot   = "🟢" if fields[2] == "ACTIVE" else "🔴"
                sensors_rows += (
                    f"<tr style='background:{color}'>"
                    f"<td>{dot} {fields[0]}</td>"
                    f"<td>{fields[1]}</td>"
                    f"<td>{fields[2]}</td>"
                    f"</tr>"
                )

    status_html = ""
    if iot_status:
        try:
            st = json.loads(iot_status)
            status_html = (
                "<div class='stats'>"
                f"<div class='stat'><span>{st.get('sensors_active','?')}</span>Sensores activos</div>"
                f"<div class='stat'><span>{st.get('sensors_total','?')}</span>Sensores totales</div>"
                f"<div class='stat'><span>{st.get('operators_connected','?')}</span>Operadores</div>"
                "</div>"
            )
        except:
            status_html = "<p>No se pudo obtener el estado.</p>"

    if not sensors_rows:
        sensors_rows = "<tr><td colspan='3'>Sin sensores registrados</td></tr>"

    return f"""<!DOCTYPE html>
<html><head><meta charset='UTF-8'>
<title>IoT Dashboard</title>
<meta http-equiv='refresh' content='30'>
{DASHBOARD_CSS}
</head><body>
<div class='header'>
  <h1>⬡ IoT Monitoring System</h1>
  <span>Usuario: <b>{username}</b> | Rol: <b>{role}</b> &nbsp;
        <a class='logout' href='/'>Cerrar sesión</a></span>
</div>
<div class='container'>
  <h3 style='color:#aaa;margin-bottom:12px'>Estado del sistema</h3>
  {status_html}
  <h3 style='color:#aaa;margin:20px 0 12px'>Sensores registrados</h3>
  <table>
    <tr><th>ID</th><th>Tipo</th><th>Estado</th></tr>
    {sensors_rows}
  </table>
  <p class='note'>Página se actualiza cada 30 segundos.</p>
</div></body></html>"""


def handle_request(conn):
    try:
        data = conn.recv(4096).decode(errors="ignore")
        if not data or len(data.strip()) == 0:
            return

        lines = data.split("\r\n")
        if not lines or len(lines[0].split(" ")) < 2:
            return

        parts  = lines[0].split(" ")
        method = parts[0]
        path   = parts[1]

        # Ignorar favicon y otros recursos del browser
        if path in ("/favicon.ico", "/robots.txt"):
            send_response(conn, 404, "Not Found", "text/plain", "")
            return

        if method == "GET" and path == "/":
            send_response(conn, 200, "OK", "text/html", get_login_html())

        elif method == "POST" and path == "/login":
            body     = data.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in data else ""
            params   = parse_qs(body)
            username = params.get("user", [""])[0]
            password = params.get("pass", [""])[0]

            auth_resp = http_get(AUTH_HOST, AUTH_PORT,
                                 f"/validate?user={username}&pass={password}")

            if auth_resp is None:
                send_response(conn, 200, "OK", "text/html",
                              get_login_html("<p class='error'>⚠ Auth service no disponible</p>"))
                return

            try:
                auth_data = json.loads(auth_resp)
            except:
                send_response(conn, 200, "OK", "text/html",
                              get_login_html("<p class='error'>⚠ Error en auth service</p>"))
                return

            if not auth_data.get("valid"):
                send_response(conn, 200, "OK", "text/html",
                              get_login_html("<p class='error'>❌ Usuario o contraseña incorrectos</p>"))
                return

            role        = auth_data.get("role", "UNKNOWN")
            iot_status  = http_get(IOT_HOST, IOT_PORT, "/status")
            iot_sensors = get_sensors_tcp()
            dashboard   = build_dashboard(username, role, iot_status, iot_sensors)
            send_response(conn, 200, "OK", "text/html", dashboard)
            print(f"[GATEWAY] Login exitoso: {username} ({role})")

        else:
            send_response(conn, 404, "Not Found", "application/json",
                          '{"error":"not found"}')
    except Exception as e:
        print(f"[GATEWAY] Error: {e}")
    finally:
        try:
            conn.close()
        except:
            pass

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, GATEWAY_PORT))
    server.listen(50)
    print(f"[GATEWAY] Web Gateway corriendo en puerto {GATEWAY_PORT}")
    print(f"[GATEWAY] Abrí http://localhost:{GATEWAY_PORT} en el browser")

    while True:
        try:
            conn, _ = server.accept()
            conn.settimeout(5)
            threading.Thread(target=handle_request, args=(conn,), daemon=True).start()
        except Exception as e:
            print(f"[GATEWAY] Error en accept: {e}")
            continue


if __name__ == "__main__":
    main()

