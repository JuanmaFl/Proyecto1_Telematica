## 1. Resumen de protocolos

- Capa de transporte principal: TCP
- Capa de aplicacion: protocolo de texto propio (sobre TCP) y HTTP/1.x
- Formatos de datos: texto plano (lineas), JSON, HTML y form-urlencoded

## 2. Mapa de puertos y servicios

- 8080/TCP: servidor IoT (canal de comandos y datos por protocolo propio)
- 8081/TCP: servidor HTTP integrado del servidor IoT
- 8082/TCP: Web Gateway
- 9090/TCP: Auth Service

## 3. Protocolo propio sobre TCP (Servidor IoT en 8080)

### 3.1 Caracteristicas

- Orientado a lineas: cada mensaje termina en salto de linea (\n)
- Mensajes legibles por humanos en texto plano
- Conexion persistente TCP para sensores y operadores
- Respuestas con codigos estilo API textual: OK o ERROR

### 3.2 Comandos de sensores

1. Registro de sensor
   - Request: REGISTER SENSOR <sensor_id> <TEMPERATURE|VIBRATION|ENERGY>
   - Response exitosa: OK REGISTERED <sensor_id>
   - Errores comunes: ERROR 400 BAD_REQUEST, ERROR 400 INVALID_SENSOR_TYPE, ERROR 409 SENSOR_ALREADY_REGISTERED

2. Envio de medicion
   - Request: DATA <sensor_id> <valor> <timestamp_unix>
   - Response exitosa: OK DATA RECEIVED
   - Errores comunes: ERROR 404 SENSOR_NOT_FOUND, ERROR 401 UNAUTHORIZED

3. Desconexion limpia
   - Request: DISCONNECT <sensor_id>
   - Response: OK DISCONNECTED <sensor_id>

### 3.3 Comandos de operadores

1. Registro de operador
   - Request: REGISTER OPERATOR <username>
   - Response: OK REGISTERED <username>

2. Listar sensores
   - Request: LIST SENSORS
   - Response: SENSORS id1:tipo1:estado1 id2:tipo2:estado2 ...
   - Sin sensores: SENSORS (none)

3. Obtener historial de un sensor
   - Request: GET DATA <sensor_id>
   - Response multilinea: DATA_RESPONSE <sensor_id> <valor> <timestamp>
   - Sin datos: DATA_RESPONSE <sensor_id> NO_DATA <timestamp>

### 3.4 Mensajes asincronos del servidor a operadores

- ALERT <sensor_id> HIGH_TEMP <valor>
- ALERT <sensor_id> HIGH_VIBRATION <valor>
- ALERT <sensor_id> HIGH_ENERGY <valor>
- SENSOR_DOWN <sensor_id>

Estos mensajes se envian por el mismo socket TCP del operador cuando se superan umbrales o se cae un sensor.

## 4. HTTP en el servidor IoT (8081)

El servidor C tambien expone un servidor HTTP basico.

### Endpoints

1. GET /
   - Retorna pagina HTML de login

2. GET /status
   - Retorna JSON con estado del sistema
   - Ejemplo:
     {"sensors_active":3,"sensors_total":5,"operators_connected":2}

3. POST /login
   - Content-Type esperado: application/x-www-form-urlencoded
   - Campos: usuario, rol
   - Respuestas JSON (exito o error)

4. Otros paths
   - 404 Not Found en JSON

## 5. Auth Service (9090) - HTTP

Servicio en Python que valida credenciales.

### Endpoint

- GET /validate?user=<usuario>&pass=<password>
- Respuesta JSON:
  - Exito: {"valid": true, "role": "OPERATOR", "username": "admin"}
  - Falla: {"valid": false, "role": null, "username": "..."}

## 6. Web Gateway (8082)

Servicio HTTP en Python que actua como puerta de enlace web.

### Flujo principal

1. Cliente web envia POST /login al gateway
2. Gateway consulta Auth Service via HTTP (9090)
3. Gateway consulta estado IoT via HTTP GET /status (8081)
4. Gateway consulta sensores via TCP al servidor IoT (8080):
   - REGISTER OPERATOR webgateway
   - LIST SENSORS
5. Gateway construye y devuelve HTML del dashboard

## 7. Operador Java

El cliente operador usa TCP hacia 8080 con dos sockets:

- Socket de comandos: LIST SENSORS, GET DATA, etc.
- Socket de alertas: escucha ALERT y SENSOR_DOWN

Nota: en la GUI existe boton STATUS que envia el comando textual STATUS por TCP, pero ese comando no esta implementado en el servidor C. El estado oficial se obtiene por HTTP en /status (puerto 8081).

## 8. Sensores Python

Cada sensor simulado usa TCP hacia 8080 y sigue este patron:

1. Conectar
2. REGISTER SENSOR ...
3. Enviar DATA periodicamente (cada SEND_INTERVAL segundos)
4. Reconectar automaticamente ante fallo de socket

## 9. Consideraciones de interoperabilidad

- El protocolo TCP propio es simple y portable, pero no versionado.
- HTTP se usa para integracion web y para el servicio de autenticacion.
- El uso de JSON en endpoints HTTP simplifica integracion con frontends.
- Para evolucion futura, convendria documentar version de API y un contrato formal (OpenAPI para HTTP y especificacion para mensajes TCP).
