package allnetui;

import java.awt.*;
import java.awt.event.ActionListener;
import java.awt.event.AdjustmentEvent;
import java.awt.event.AdjustmentListener;
import javax.swing.*;
import javax.swing.border.Border;
import javax.swing.border.LineBorder;
import utils.HtmlLabel;

/**
 *
 * @author Henry
 */
class ConversationPanel extends JPanel {

    // just to avoid a warning
    private static final long serialVersionUID = 1L;
    //
    // define this panel's command here; later we should move all commands to one place
    public static final String SEND_COMMAND = "SEND";
    public static final String CLOSE_COMMAND = "CLOSE";
    public static final String CONTACTS_COMMAND = "CONTACTS";
    public static final String EXCHANGE_KEYS_COMMAND = "EXCHANGE_KEYS";
    //
    private String contactName;
    private JPanel messagePanel;
    private HtmlLabel topLabel;
    private JScrollPane scrollPane;
    private boolean scrollToBottom;
    private JTextField msgField;
    // the buttons
    private JButton close, exchangeKeys, goToContacts, send;
    // the command prefix will identify which instance of the Class is sending the event
    private String commandPrefix;
    // default colors to use
    private static Color background = Color.GRAY, foreground = Color.WHITE;
    private static Color broadcastColor = Color.LIGHT_GRAY;

    ConversationPanel(String info, String commandPrefix, String contactName) {
        this.commandPrefix = commandPrefix;
        this.contactName = contactName;
        setBackground(background);
        // make the info label for the top of the panel
        topLabel = new HtmlLabel(info);
        topLabel.setOpaque(true);
        topLabel.setBackground(foreground);
        topLabel.setLineBorder(Color.BLACK, 1, false);

        // make the button panel
        JPanel buttonPanel = makeButtonPanel();

        // make the panel to hold the messages
        messagePanel = makeMessagePanel(background);
        scrollPane = new JScrollPane(messagePanel,
                ScrollPaneConstants.VERTICAL_SCROLLBAR_AS_NEEDED,
                ScrollPaneConstants.HORIZONTAL_SCROLLBAR_NEVER);
        // must set a min and preferred size for the scroll pane
        Dimension scrDim = new Dimension(250, 1500);
        scrollPane.setMinimumSize(scrDim);
        scrollPane.setPreferredSize(scrDim);
        // so why not set max as well...
        scrollPane.setMaximumSize(scrDim);
        // don't want a border around it
        scrollPane.setBorder(BorderFactory.createEmptyBorder());
        // make it scroll to the bottom when we add something
        scrollPane.getVerticalScrollBar().addAdjustmentListener(new MyAdjustmentListener());
        //
        // make the text input components
        msgField = new JTextField();
        msgField.setActionCommand(commandPrefix + ":" + SEND_COMMAND);
        send = makeButton("Send", SEND_COMMAND);
        // now add these components to our panel
        setLayout(new GridBagLayout());
        GridBagConstraints gbc = new GridBagConstraints();
        gbc.fill = GridBagConstraints.HORIZONTAL;
        gbc.weightx = 1.0;
        gbc.weighty = 0.0;
        gbc.insets = new Insets(5, 5, 5, 5);
        gbc.anchor = GridBagConstraints.CENTER;
        gbc.gridx = 0;
        gbc.gridy = 0;
        gbc.gridheight = 1;
        gbc.gridwidth = 3;
        add(topLabel, gbc);
        gbc.gridy++;
        gbc.weighty = 1.0;
        gbc.anchor = GridBagConstraints.PAGE_START;
        add(scrollPane, gbc);
        gbc.anchor = GridBagConstraints.CENTER;
        gbc.gridy++;
        gbc.weighty = 0.0;
        gbc.weightx = 1.0;
        gbc.gridwidth = 2;
        add(msgField, gbc);
        gbc.gridwidth = 1;
        gbc.gridx = 2;
        gbc.weightx = 0.0;
        add(send, gbc);
        gbc.gridy++;
        gbc.gridwidth = 3;
        gbc.gridx = 0;
        add(buttonPanel, gbc);
    }

    public String getContactName() {
        return contactName;
    }

    public String getMsgToSend() {
        String msg = msgField.getText();
        msgField.setText("");
        return (msg);
    }

    static void setDefaultColors(Color background, Color foreground, Color broadcastColor) {
        ConversationPanel.background = background;
        ConversationPanel.foreground = foreground;
        ConversationPanel.broadcastColor = broadcastColor;
    }

    void setListener(ActionListener listener) {
        close.addActionListener(listener);
        exchangeKeys.addActionListener(listener);
        goToContacts.addActionListener(listener);
        send.addActionListener(listener);
        // send event when return key is entered
        msgField.addActionListener(listener);
    }

    private JPanel makeButtonPanel() {
        close = makeButton("Close", CLOSE_COMMAND);
        exchangeKeys = makeButton("Exchange Keys", EXCHANGE_KEYS_COMMAND);
        goToContacts = makeButton("Contacts", CONTACTS_COMMAND);
        Insets insets = new Insets(2, 8, 2, 8);
        close.setMargin(insets);
        exchangeKeys.setMargin(insets);
        goToContacts.setMargin(insets);
        JPanel panel = new JPanel();
        panel.setBackground(background);
        panel.setLayout(new BoxLayout(panel, BoxLayout.X_AXIS));
        // panel.add(Box.createHorizontalGlue());
// exchanging keys is now done through "New Contact"
//        panel.add(exchangeKeys);
        panel.add(Box.createHorizontalGlue());
        panel.add(close);
        panel.add(Box.createHorizontalGlue());
        panel.add(goToContacts);
        // panel.add(Box.createHorizontalGlue());
        // panel.setBorder(new LineBorder(Color.BLACK, 1));
        return (panel);
    }

    private JButton makeButton(String text, String command) {
        JButton button = new JButton(text);
        button.setActionCommand(commandPrefix + ":" + command);
        return (button);
    }

    private JPanel makeMessagePanel(Color background) {
        JPanel panel = new JPanel();
        panel.setLayout(new BoxLayout(panel, BoxLayout.Y_AXIS));
        panel.setBackground(background);
        return (panel);
    }

    void addMsg(String text, boolean left, boolean broadcast) {
        String[] lines = text.split("\n");
        JPanel bubble = makeBubble(left, broadcast ? broadcastColor : Color.WHITE, lines);
        JPanel inner = new JPanel();
        inner.setBackground(background);
        inner.setLayout(new BoxLayout(inner, BoxLayout.X_AXIS));
        if (left) {
            inner.add(bubble);
            inner.add(Box.createHorizontalGlue());
        }
        else {
            inner.add(Box.createHorizontalGlue());
            inner.add(bubble);
        }
        messagePanel.add(inner);
        messagePanel.add(Box.createRigidArea(new Dimension(0, 4)));
        // tell scroll panel to scroll to the bottom the next time it adjusts,
        // which will be triggered right now when it validates.  there is 
        // apparently no other way to do this 
        scrollToBottom = true;
        messagePanel.revalidate();
    }

    private JPanel makeBubble(boolean leftJustified, Color color, String... lines) {
        JPanel panel = new JPanel();
        panel.setBackground(color);
        panel.setLayout(new BoxLayout(panel, BoxLayout.Y_AXIS));
        JLabel label;
        for (int i = 0; i < lines.length; i++) {
            label = new JLabel(lines[i]);
            label.setOpaque(true);
            label.setBackground(color);
            if (leftJustified) {
                label.setAlignmentX(Component.LEFT_ALIGNMENT);
            }
            else {
                label.setAlignmentX(Component.RIGHT_ALIGNMENT);
            }
            panel.add(label);
        }
        Border compound = BorderFactory.createCompoundBorder(new LineBorder(Color.BLACK, 1, true), new LineBorder(color, 2, true));
        // panel.setBorder(new LineBorder(Color.BLACK, 1, true));
        panel.setBorder(compound);
        return (panel);
    }

    // not used: text area doesn't size correctly inside a scroll pane
    private JTextArea makeBubble1(String... lines) {
        JTextArea panel = new JTextArea();
        panel.setBackground(Color.WHITE);
        panel.setLayout(new BoxLayout(panel, BoxLayout.Y_AXIS));
        JLabel label;
        for (int i = 0; i < lines.length; i++) {
            label = new JLabel(lines[i]);
            label.setOpaque(true);
            label.setBackground(Color.WHITE);
            panel.add(label);
        }
        Border compound = BorderFactory.createCompoundBorder(new LineBorder(Color.BLACK, 1, true), new LineBorder(Color.WHITE, 2, true));
        panel.setBorder(compound);
        return (panel);
    }

    void removeAllMsgs() {
        messagePanel.removeAll();
        validate();
    }

    void setTopLabelText(String... lines) {
        topLabel.setText(lines);
    }

    private class MyAdjustmentListener implements AdjustmentListener {

        private MyAdjustmentListener() {
        }

        @Override
        public void adjustmentValueChanged(AdjustmentEvent e) {
            if (scrollToBottom) {
                scrollToBottom = false;
                e.getAdjustable().setValue(e.getAdjustable().getMaximum());
            }
        }
    }
}
