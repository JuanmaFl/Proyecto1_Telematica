import java.io.*;
import java.net.*;

public class ServerConnection {
    private String host;
    private int port;

    // Conexión para comandos (LIST SENSORS, STATUS, etc.)
    private Socket commandSocket;
    private BufferedReader commandReader;
    private PrintWriter commandWriter;

    // Conexión dedicada solo a recibir alertas
    private Socket alertSocket;
    private BufferedReader alertReader;
    private PrintWriter alertWriter;

    public ServerConnection(String host, int port) {
        this.host = host;
        this.port = port;
    }

    public boolean connect(String username) {
        try {
            // Conexión principal para comandos
            commandSocket = new Socket(host, port);
            commandReader = new BufferedReader(new InputStreamReader(commandSocket.getInputStream()));
            commandWriter = new PrintWriter(commandSocket.getOutputStream(), true);

            commandWriter.println("REGISTER OPERATOR " + username + "_cmd");
            String response = commandReader.readLine();
            System.out.println("Registro cmd: " + response);

            // Conexión secundaria solo para alertas
            alertSocket = new Socket(host, port);
            alertReader = new BufferedReader(new InputStreamReader(alertSocket.getInputStream()));
            alertWriter = new PrintWriter(alertSocket.getOutputStream(), true);

            alertWriter.println("REGISTER OPERATOR " + username + "_alerts");
            String alertResponse = alertReader.readLine();
            System.out.println("Registro alerts: " + alertResponse);

            return response != null && response.startsWith("OK");

        } catch (IOException e) {
            System.out.println("Error de conexión: " + e.getMessage());
            return false;
        }
    }

    public String sendCommand(String command) {
        try {
            commandWriter.println(command);
            return commandReader.readLine();
        } catch (IOException e) {
            return "ERROR conexión perdida";
        }
    }

    public void listenForAlerts(AlertListener listener) {
        Thread t = new Thread(() -> {
            try {
                String line;
                while ((line = alertReader.readLine()) != null) {
                    if (line.startsWith("ALERT") || line.startsWith("SENSOR_DOWN")) {
                        listener.onAlert(line);
                    }
                }
            } catch (IOException e) {
                listener.onAlert("CONEXIÓN PERDIDA CON EL SERVIDOR");
            }
        });
        t.setDaemon(true);
        t.start();
    }

    public interface AlertListener {
        void onAlert(String message);
    }

    public void close() {
        try {
            if (commandSocket != null) commandSocket.close();
            if (alertSocket != null) alertSocket.close();
        } catch (IOException e) {
            System.out.println("Error al cerrar: " + e.getMessage());
        }
    }
}
