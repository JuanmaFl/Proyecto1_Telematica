import javax.swing.*;
import javax.swing.table.*;
import java.awt.*;

public class OperatorGUI extends JFrame {
    private ServerConnection connection;
    private DefaultTableModel sensorsTableModel;
    private DefaultListModel<String> alertsListModel;
    private JTable sensorsTable;

    public OperatorGUI(ServerConnection connection) {
        this.connection = connection;
        buildUI();
        startAutoRefresh();
        connection.listenForAlerts(this::addAlert);
    }

    private void buildUI() {
        setTitle("IoT Monitoring — Operador");
        setSize(900, 600);
        setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);

        JPanel root = new JPanel(new BorderLayout(5, 5));
        root.setBorder(BorderFactory.createEmptyBorder(5, 5, 5, 5));
        setContentPane(root);

        // ── Barra superior ──
        JLabel statusLabel = new JLabel("Conectado al servidor");
        statusLabel.setForeground(new Color(0, 130, 0));
        JButton refreshBtn = new JButton("Actualizar");
        refreshBtn.addActionListener(e -> refreshSensors());
        JPanel topPanel = new JPanel(new BorderLayout());
        topPanel.add(statusLabel, BorderLayout.WEST);
        topPanel.add(refreshBtn, BorderLayout.EAST);
        root.add(topPanel, BorderLayout.NORTH);

        // ── Centro: tabla + alertas ──
        JPanel centerPanel = new JPanel(new GridLayout(1, 2, 5, 0));

        String[] cols = {"ID", "Tipo", "Estado"};
        sensorsTableModel = new DefaultTableModel(cols, 0) {
            public boolean isCellEditable(int r, int c) { return false; }
        };
        sensorsTable = new JTable(sensorsTableModel);
        sensorsTable.setRowHeight(26);
        JScrollPane tableScroll = new JScrollPane(sensorsTable);
        tableScroll.setBorder(BorderFactory.createTitledBorder("Sensores activos"));
        centerPanel.add(tableScroll);

        alertsListModel = new DefaultListModel<>();
        JList<String> alertsList = new JList<>(alertsListModel);
        alertsList.setFont(new Font("Monospaced", Font.PLAIN, 12));
        JScrollPane alertsScroll = new JScrollPane(alertsList);
        alertsScroll.setBorder(BorderFactory.createTitledBorder("Alertas"));
        centerPanel.add(alertsScroll);

        root.add(centerPanel, BorderLayout.CENTER);

        // ── Botones inferiores ──
        JButton listBtn = new JButton("LIST SENSORS");
        JButton statusBtn = new JButton("STATUS");
        listBtn.addActionListener(e -> refreshSensors());
        statusBtn.addActionListener(e -> queryStatus());
        JPanel bottomPanel = new JPanel(new FlowLayout(FlowLayout.LEFT));
        bottomPanel.add(listBtn);
        bottomPanel.add(statusBtn);
        root.add(bottomPanel, BorderLayout.SOUTH);

        setLocationRelativeTo(null);
        setVisible(true);
    }

    public void refreshSensors() {
        String response = connection.sendCommand("LIST SENSORS");
        if (response == null) return;
        SwingUtilities.invokeLater(() -> {
            int selectedRow = sensorsTable.getSelectedRow();
            sensorsTableModel.setRowCount(0);
            if (response.startsWith("SENSORS")) {
                String[] parts = response.substring(7).trim().split(" ");
                for (String part : parts) {
                    if (part.equals("(none)") || part.isEmpty()) continue;
                    String[] fields = part.split(":");
                    if (fields.length >= 3)
                        sensorsTableModel.addRow(new Object[]{fields[0], fields[1], fields[2]});
                }
            }
            if (selectedRow >= 0 && selectedRow < sensorsTableModel.getRowCount())
                sensorsTable.setRowSelectionInterval(selectedRow, selectedRow);
        });
    }

    private void queryStatus() {
        String response = connection.sendCommand("STATUS");
        JOptionPane.showMessageDialog(this, response, "Estado", JOptionPane.INFORMATION_MESSAGE);
    }

    public void addAlert(String alert) {
        SwingUtilities.invokeLater(() -> {
            alertsListModel.add(0, alert);
            if (alertsListModel.size() > 100)
                alertsListModel.remove(alertsListModel.size() - 1);
        });
    }

    private void startAutoRefresh() {
        new Timer(5000, e -> refreshSensors()).start();
    }
}
