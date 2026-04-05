import javax.swing.*;
import javax.swing.table.*;
import java.awt.*;
import java.util.ArrayList;
import java.util.List;

// Este es el main del cliente operador, se encarga de pedir el nombre del operador, conectarse al servidor y lanzar la interfaz grafica.
public class OperatorGUI extends JFrame {
    private ServerConnection connection;
    private DefaultTableModel sensorsTableModel;
    private DefaultListModel<String> alertsListModel;
    private JTable sensorsTable;
    private ChartPanel chartPanel;
    private JLabel totalSensorsLabel;
    private JLabel totalAlertsLabel;
    private int alertCount = 0;

    private final Color BG_DARK   = new Color(18, 18, 28);
    private final Color BG_PANEL  = new Color(28, 28, 42);
    private final Color BG_HEADER = new Color(35, 35, 55);
    private final Color ACCENT    = new Color(100, 180, 255);
    private final Color TEXT_MAIN = new Color(220, 220, 235);
    private final Color TEXT_DIM  = new Color(130, 130, 160);
    private final Color ALERT_CLR = new Color(255, 100, 100);
    private final Color OK_CLR    = new Color(80, 200, 120);

    public OperatorGUI(ServerConnection connection) {
        this.connection = connection;
        buildUI();
        startAutoRefresh();
        connection.listenForAlerts(this::addAlert);
    }

    private void buildUI() {
        setTitle("IoT Monitoring System — Panel de Operador");
        setSize(1100, 700);
        setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        setBackground(BG_DARK);

        JPanel root = new JPanel(new BorderLayout(0, 0));
        root.setBackground(BG_DARK);
        setContentPane(root);

        root.add(buildTopBar(),    BorderLayout.NORTH);
        root.add(buildCenter(),    BorderLayout.CENTER);
        root.add(buildBottomBar(), BorderLayout.SOUTH);

        setLocationRelativeTo(null);
        setVisible(true);
    }
    // Aqui se construye la barra superior con el titulo, estadisticas y boton de actualizar.
    private JPanel buildTopBar() {
        JPanel bar = new JPanel(new BorderLayout());
        bar.setBackground(BG_HEADER);
        bar.setBorder(BorderFactory.createEmptyBorder(12, 16, 12, 16));

        JLabel title = new JLabel("⬡  IoT Monitoring System");
        title.setFont(new Font("Arial", Font.BOLD, 18));
        title.setForeground(ACCENT);

        JPanel statsPanel = new JPanel(new FlowLayout(FlowLayout.RIGHT, 20, 0));
        statsPanel.setBackground(BG_HEADER);

        totalSensorsLabel = makeStatLabel("Sensores: 0");
        totalAlertsLabel  = makeStatLabel("Alertas: 0");
        JLabel connLabel  = makeStatLabel("● Conectado");
        connLabel.setForeground(OK_CLR);

        statsPanel.add(totalSensorsLabel);
        statsPanel.add(totalAlertsLabel);
        statsPanel.add(connLabel);

        JButton refreshBtn = new JButton("↻ Actualizar");
        refreshBtn.setBackground(ACCENT);
        refreshBtn.setForeground(BG_DARK);
        refreshBtn.setFocusPainted(false);
        refreshBtn.setFont(new Font("Arial", Font.BOLD, 12));
        refreshBtn.setBorder(BorderFactory.createEmptyBorder(6, 14, 6, 14));
        refreshBtn.addActionListener(e -> refreshSensors());
        statsPanel.add(refreshBtn);

        bar.add(title,      BorderLayout.WEST);
        bar.add(statsPanel, BorderLayout.EAST);
        return bar;
    }

    // Metodo auxiliar para crear etiquetas de estadisticas.
    private JLabel makeStatLabel(String text) {
        JLabel l = new JLabel(text);
        l.setFont(new Font("Arial", Font.PLAIN, 13));
        l.setForeground(TEXT_DIM);
        return l;
    }

    // Aqui se construye el panel central con la tabla de sensores a la izquierda y las alertas a la derecha.
    private JPanel buildCenter() {
        JPanel center = new JPanel(new GridLayout(1, 2, 6, 0));
        center.setBackground(BG_DARK);
        center.setBorder(BorderFactory.createEmptyBorder(6, 6, 6, 6));
        center.add(buildLeftPanel());
        center.add(buildRightPanel());
        return center;
    }

    // Aqui se contruye el panel izquierdo.
    private JPanel buildLeftPanel() {
        JPanel panel = new JPanel(new BorderLayout(0, 6));
        panel.setBackground(BG_DARK);

        String[] cols = {"ID", "Tipo", "Estado", "Último valor"};
        sensorsTableModel = new DefaultTableModel(cols, 0) {
            public boolean isCellEditable(int r, int c) { return false; }
        };
        sensorsTable = new JTable(sensorsTableModel) {
            public Component prepareRenderer(TableCellRenderer r, int row, int col) {
                Component c = super.prepareRenderer(r, row, col);
                String estado = (String) getModel().getValueAt(row, 2);
                c.setBackground("ACTIVE".equals(estado)
                    ? new Color(30, 50, 35)
                    : new Color(50, 30, 30));
                c.setForeground(TEXT_MAIN);
                if (isRowSelected(row)) c.setBackground(new Color(50, 80, 120));
                return c;
            }
        };
        sensorsTable.setBackground(BG_PANEL);
        sensorsTable.setForeground(TEXT_MAIN);
        sensorsTable.setRowHeight(30);
        sensorsTable.setShowGrid(false);
        sensorsTable.setIntercellSpacing(new Dimension(0, 2));
        sensorsTable.getTableHeader().setBackground(BG_HEADER);
        sensorsTable.getTableHeader().setForeground(ACCENT);
        sensorsTable.getTableHeader().setFont(new Font("Arial", Font.BOLD, 13));
        sensorsTable.setFont(new Font("Arial", Font.PLAIN, 13));

        JScrollPane tableScroll = new JScrollPane(sensorsTable);
        tableScroll.setBackground(BG_PANEL);
        tableScroll.getViewport().setBackground(BG_PANEL);
        tableScroll.setBorder(BorderFactory.createTitledBorder(
            BorderFactory.createLineBorder(new Color(60, 60, 90)),
            "  Sensores activos", 0, 0,
            new Font("Arial", Font.BOLD, 13), ACCENT));

        chartPanel = new ChartPanel();
        chartPanel.setBorder(BorderFactory.createTitledBorder(
            BorderFactory.createLineBorder(new Color(60, 60, 90)),
            "  Mediciones en tiempo real", 0, 0,
            new Font("Arial", Font.BOLD, 13), ACCENT));
        chartPanel.setPreferredSize(new Dimension(0, 220));

        panel.add(tableScroll, BorderLayout.CENTER);
        panel.add(chartPanel,  BorderLayout.SOUTH);
        return panel;
    }

    // Aqui se contruye el panel derecho.
    private JPanel buildRightPanel() {
        JPanel panel = new JPanel(new BorderLayout());
        panel.setBackground(BG_DARK);

        alertsListModel = new DefaultListModel<>();
        JList<String> alertsList = new JList<>(alertsListModel);
        alertsList.setBackground(BG_PANEL);
        alertsList.setForeground(ALERT_CLR);
        alertsList.setFont(new Font("Monospaced", Font.PLAIN, 12));
        alertsList.setSelectionBackground(new Color(80, 30, 30));
        alertsList.setCellRenderer(new DefaultListCellRenderer() {
            public Component getListCellRendererComponent(JList<?> list, Object value,
                    int index, boolean isSelected, boolean cellHasFocus) {
                JLabel l = (JLabel) super.getListCellRendererComponent(
                        list, value, index, isSelected, cellHasFocus);
                String text = value.toString();
                l.setBackground(isSelected ? new Color(80, 30, 30)
                    : (index % 2 == 0 ? BG_PANEL : new Color(33, 33, 50)));
                l.setForeground(text.contains("SENSOR_DOWN") ? TEXT_DIM : ALERT_CLR);
                l.setBorder(BorderFactory.createEmptyBorder(4, 8, 4, 8));
                return l;
            }
        });

        JScrollPane alertsScroll = new JScrollPane(alertsList);
        alertsScroll.getViewport().setBackground(BG_PANEL);
        alertsScroll.setBorder(BorderFactory.createTitledBorder(
            BorderFactory.createLineBorder(new Color(60, 60, 90)),
            "  Alertas en tiempo real", 0, 0,
            new Font("Arial", Font.BOLD, 13), ALERT_CLR));

        panel.add(alertsScroll, BorderLayout.CENTER);
        return panel;
    }
    // Aqui construimos la barra inferior con botones para listar sensores y consultar el estado del sistema.
    private JPanel buildBottomBar() {
        JPanel bar = new JPanel(new FlowLayout(FlowLayout.LEFT, 10, 8));
        bar.setBackground(BG_HEADER);

        JButton listBtn   = makeBtn("LIST SENSORS", ACCENT);
        JButton statusBtn = makeBtn("STATUS", new Color(100, 255, 160));
        listBtn.addActionListener(e -> refreshSensors());
        statusBtn.addActionListener(e -> queryStatus());

        bar.add(listBtn);
        bar.add(statusBtn);
        return bar;
    }

    private JButton makeBtn(String text, Color color) {
        JButton b = new JButton(text);
        b.setBackground(new Color(40, 40, 60));
        b.setForeground(color);
        b.setFocusPainted(false);
        b.setFont(new Font("Arial", Font.BOLD, 12));
        b.setBorder(BorderFactory.createCompoundBorder(
            BorderFactory.createLineBorder(color, 1),
            BorderFactory.createEmptyBorder(6, 14, 6, 14)));
        return b;
    }
    // Este metodo se encarga de enviar el comando para listar los sensores, parsear la respuesta y actualizar la tabla y el panel de graficos.
    public void refreshSensors() {
        new Thread(() -> {
            String response = connection.sendCommand("LIST SENSORS");
            if (response == null) return;

            List<Object[]> rows = new ArrayList<>();

            if (response.startsWith("SENSORS")) {
                String[] parts = response.substring(7).trim().split(" ");
                for (String part : parts) {
                    if (part.equals("(none)") || part.isEmpty()) continue;
                    String[] fields = part.split(":");
                    if (fields.length >= 3) {
                        String lastVal = "—";
                        if (fields[2].equals("ACTIVE")) {
                            String dataResp = connection.sendCommandMultiLine("GET DATA " + fields[0]);
                            lastVal = parseLastValue(dataResp);
                        }
                        rows.add(new Object[]{fields[0], fields[1], fields[2], lastVal});

                        if (!lastVal.equals("—")) {
                            try {
                                float val = Float.parseFloat(lastVal.split(" ")[0]);
                                chartPanel.updateSensor(fields[0], fields[1], val);
                            } catch (NumberFormatException ignored) {}
                        }
                    }
                }
            }

            final int count = rows.size();
            SwingUtilities.invokeLater(() -> {
                int selectedRow = sensorsTable.getSelectedRow();
                sensorsTableModel.setRowCount(0);
                for (Object[] row : rows)
                    sensorsTableModel.addRow(row);
                totalSensorsLabel.setText("Sensores: " + count);
                if (selectedRow >= 0 && selectedRow < sensorsTableModel.getRowCount())
                    sensorsTable.setRowSelectionInterval(selectedRow, selectedRow);
            });
        }).start();
    }
    // Este metodo se encarga de enviar el comando para consultar el estado del sistema y mostrar la respuesta.
    private String parseLastValue(String response) {
        if (response == null || response.contains("NO_DATA") || response.contains("ERROR")) return "—";
        String[] lines = response.trim().split("\n");
        String last = lines[lines.length - 1];
        String[] parts = last.trim().split("\\s+");
        if (parts.length >= 3) return parts[2];
        return "—";
    }
    // Este metodo se encarga de enviar el comando para consultar el estado del sistema y mostrar la respuesta.
    private void queryStatus() {
        String response = connection.sendCommand("STATUS");
        JOptionPane.showMessageDialog(this, response, "Estado del sistema",
                JOptionPane.INFORMATION_MESSAGE);
    }
    // Aqui nos encargamos de agregar una nueva alerta a la lista de alertas en tiempo real.
    public void addAlert(String alert) {
        SwingUtilities.invokeLater(() -> {
            alertCount++;
            totalAlertsLabel.setText("Alertas: " + alertCount);
            alertsListModel.add(0, alert);
            if (alertsListModel.size() > 200)
                alertsListModel.remove(alertsListModel.size() - 1);
        });
    }

    private void startAutoRefresh() {
        new Timer(5000, e -> refreshSensors()).start();
    }
}
