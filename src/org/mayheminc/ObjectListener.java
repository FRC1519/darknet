package org.mayheminc;

import java.util.*;
import java.net.*;
import java.io.*;
import java.nio.*;
import org.mayheminc.ObjectLocation;

public class ObjectListener extends Thread {
    protected static final int MAX_OBJECTS_PER_FRAME = 20;
    protected static final int MAYHEM_MAGIC = 0x1519B0B4;
    protected static final int MAX_BUFFER = 1500;
    protected static final int DEFAULT_PORT = 5810;

    private DatagramSocket socket;
    private DatagramPacket packet;
    private ByteBuffer buffer;
    private int lastFrame = 0;
    private ArrayList<ObjectLocation> objList;

    public ObjectListener() throws SocketException {
        this(DEFAULT_PORT);
    }

    public ObjectListener(int port) throws SocketException {
        super("ObjectListener-" + port);

        socket = new DatagramSocket(port);

        byte[] byteBuffer = new byte[MAX_BUFFER];
        packet = new DatagramPacket(byteBuffer, byteBuffer.length);
        buffer = ByteBuffer.wrap(byteBuffer);
    }

    public int getLastFrame() {
        return lastFrame;
    }

    public List getObjectList() {
        return objList;
    }

    public void run() {
        long lastTimestamp = 0;

        System.out.println("Running and awaiting packets...\n"); // TODO Remove

        while (true) {
            try {
                // Receive new datagram
                socket.receive(packet);
                buffer.rewind();
            } catch (IOException e) {
                System.err.println(super.getName() + " encountered an error");
                e.printStackTrace();
                System.err.println(super.getName() + " aborting");
                break;
            }

            // Validate packet
            int magic = buffer.getInt();
            if (magic != MAYHEM_MAGIC) {
                System.err.println("Invalid packet received (magic == 0x" + Integer.toHexString(magic) + ")");
                continue;
            }

            // Get information about the update
            int frame = buffer.getInt();
            long timestamp = buffer.getLong();

            // Check for out-of-date data
            if (frame <= lastFrame) {
                System.err.println("Rejecting older frame #" + frame + " (already have frame #" + lastFrame + ")");
                continue;
            }
            if (timestamp <= lastTimestamp) {
                System.err.println("Oddly, timestamp for new frame #" + frame + " (" + timestamp + ") is not newer than that for previous frame #" + lastFrame + " (" + lastTimestamp + ")");
            }

            // TODO Remove
            System.out.println("Received frame " + frame + " sent at " + timestamp);

            // Get list of all objects involved
            ArrayList<ObjectLocation> objList = new ArrayList<ObjectLocation>();
            for (int i = 0; i < MAX_OBJECTS_PER_FRAME; i++) {
                ObjectLocation loc = new ObjectLocation(buffer);
                objList.add(loc);

                System.out.println("Object found: " + loc); // TODO Remove
            }

            // Update the list of objects
            this.objList = objList;
            lastFrame = frame;
            lastTimestamp = timestamp;
        }

        // Clean up
        socket.close();
    }

    public static void main(String[] args) {
        ObjectListener listener;


        try {
            listener = new ObjectListener();
        } catch (SocketException e) {
            e.printStackTrace();
            return;
        }

        listener.start();

        // TODO Iterate over results and print them out
    }
}
