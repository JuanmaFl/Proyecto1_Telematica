import javax.swing.*;
import java.awt.*;
import java.util.*;

// Este es el panel personalizado para mostrar gráficos de sensores en tiempo real.

public class ChartPanel extends JPanel {
    private final Map<String, float[]> sensorHistory = new LinkedHashMap<>();
    private final Map<String, String> sensorTypes = new LinkedHashMap<>();
    private static final int MAX_POINTS = 10;

    private final Color BG       = new Color(18, 18, 28);
    private final Color GRID     = new Color(50, 50, 70);
    private final Color TEXT_CLR = new Color(180, 180, 200);
    private final Color[] COLORS = {
        new Color(100, 180, 255),
        new Color(100, 255, 160),
        new Color(255, 180, 80),
        new Color(255, 100, 130),
        new Color(180, 120, 255)
    };

    // Constructuor del panel 
    public ChartPanel() {
        setBackground(new Color(18, 18, 28));
        setPreferredSize(new Dimension(400, 200));
    }
    //  Metodo que se usa para actualizar los datos de un sensor y se guardna los utlimos valores para graficar.
    public void updateSensor(String id, String type, float value) {
        sensorTypes.put(id, type);
        float[] history = sensorHistory.getOrDefault(id, new float[0]);
        float[] newHistory = new float[Math.min(history.length + 1, MAX_POINTS)];
        int srcPos = Math.max(0, history.length - MAX_POINTS + 1);
        System.arraycopy(history, srcPos, newHistory, 0, newHistory.length - 1);
        newHistory[newHistory.length - 1] = value;
        sensorHistory.put(id, newHistory);
        repaint();
    }

    //  Aqui se dibuja el panel y los graficos.
    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);
        Graphics2D g2 = (Graphics2D) g;
        g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);

        int w = getWidth();
        int h = getHeight();
        int pad = 45;

        // Fondo
        g2.setColor(BG);
        g2.fillRect(0, 0, w, h);

        if (sensorHistory.isEmpty()) {
            g2.setColor(TEXT_CLR);
            g2.setFont(new Font("Arial", Font.PLAIN, 13));
            g2.drawString("Esperando datos...", w / 2 - 60, h / 2);
            return;
        }

        // Calcular min/max global
        float minVal = Float.MAX_VALUE, maxVal = Float.MIN_VALUE;
        for (float[] hist : sensorHistory.values()) {
            for (float v : hist) {
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
            }
        }
        if (maxVal == minVal) maxVal = minVal + 1;
        float range = maxVal - minVal;

        // Grid lines
        g2.setColor(GRID);
        g2.setStroke(new BasicStroke(0.5f));
        int gridLines = 4;
        for (int i = 0; i <= gridLines; i++) {
            int y = pad + (int) ((h - 2 * pad) * i / gridLines);
            g2.drawLine(pad, y, w - 10, y);
            float val = maxVal - (range * i / gridLines);
            g2.setColor(TEXT_CLR);
            g2.setFont(new Font("Arial", Font.PLAIN, 10));
            g2.drawString(String.format("%.0f", val), 2, y + 4);
            g2.setColor(GRID);
        }

        // Líneas por sensor
        int colorIdx = 0;
        java.util.List<String> ids = new ArrayList<>(sensorHistory.keySet());
        for (String id : ids) {
            float[] hist = sensorHistory.get(id);
            if (hist.length < 2) { colorIdx++; continue; }

            Color c = COLORS[colorIdx % COLORS.length];
            g2.setColor(c);
            g2.setStroke(new BasicStroke(2f));

            int chartW = w - pad - 10;
            int chartH = h - 2 * pad;

            for (int i = 1; i < hist.length; i++) {
                int x1 = pad + (int) ((i - 1) * chartW / (MAX_POINTS - 1));
                int y1 = pad + chartH - (int) ((hist[i - 1] - minVal) * chartH / range);
                int x2 = pad + (int) (i * chartW / (MAX_POINTS - 1));
                int y2 = pad + chartH - (int) ((hist[i] - minVal) * chartH / range);
                g2.drawLine(x1, y1, x2, y2);
                // Punto
                g2.fillOval(x2 - 3, y2 - 3, 6, 6);
            }
            colorIdx++;
        }

        // Leyenda
        colorIdx = 0;
        int legendX = pad;
        for (String id : ids) {
            Color c = COLORS[colorIdx % COLORS.length];
            g2.setColor(c);
            g2.fillRect(legendX, h - 14, 10, 10);
            g2.setFont(new Font("Arial", Font.PLAIN, 10));
            g2.drawString(id, legendX + 13, h - 5);
            legendX += g2.getFontMetrics().stringWidth(id) + 30;
            colorIdx++;
        }
    }
}