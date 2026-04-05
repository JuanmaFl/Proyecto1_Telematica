import javax.swing.*;

public class Main {
    private static final String SERVER_HOST = "iot-telematica.duckdns.org";
    private static final int SERVER_PORT = 8080;

    public static void main(String[] args) {
        String username = JOptionPane.showInputDialog(
                null,
                "Ingresá tu nombre de operador:",
                "IoT Monitoring — Login",
                JOptionPane.QUESTION_MESSAGE
        );

        if (username == null || username.trim().isEmpty()) {
            System.out.println("No se ingresó usuario. Saliendo.");
            System.exit(0);
        }

        ServerConnection connection = new ServerConnection(SERVER_HOST, SERVER_PORT);
        boolean connected = connection.connect(username.trim());

        if (!connected) {
            JOptionPane.showMessageDialog(null,
                    "No se pudo conectar al servidor.",
                    "Error de conexión",
                    JOptionPane.ERROR_MESSAGE);
            System.exit(1);
        }

        SwingUtilities.invokeLater(() -> {
            try {
                UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName());
            } catch (Exception e) {
                e.printStackTrace();
            }
            OperatorGUI gui = new OperatorGUI(connection);
            gui.refreshSensors();
        });
    }
}
