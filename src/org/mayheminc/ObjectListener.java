package org.mayheminc;

import java.net.*;
import java.io.*;
import java.nio.*;
import org.mayheminc.ObjectLocation;

public class ObjectListener extends Thread {

    protected static final int MAX_OBJECTS_PER_FRAME = 20;

    protected static final int MAYHEM_MAGIC = 0x1519B0B4;

    protected static final int MAX_BUFFER = 1500;

    protected static final int DEFAULT_PORT = 5810;

    public static void main(String[] args) {
        System.out.println("Hello, world!");

        DatagramSocket socket = null;

        try {
            socket = new DatagramSocket(DEFAULT_PORT);
        } catch (SocketException e) {
            e.printStackTrace();
            return;
        }

        byte[] buf = new byte[MAX_BUFFER];
        DatagramPacket packet = new DatagramPacket(buf, buf.length);
        ByteBuffer wrapped = ByteBuffer.wrap(buf);
        while (true) {
            try {
                socket.receive(packet);
                wrapped.rewind();

                int magic = wrapped.getInt();
                if (magic != MAYHEM_MAGIC) {
                    System.err.println("Invalid packet received (magic == 0x" + Integer.toHexString(magic) + ")");
                    continue;
                }

                int frame = wrapped.getInt();
                long timestamp = wrapped.getLong();
                /* TODO Reject older things */

                ObjectLocation loc = new ObjectLocation();

                System.out.println("Received frame " + frame + " sent at " + timestamp);
                for (int i = 0; i < MAX_OBJECTS_PER_FRAME; i++) {
                    loc = new ObjectLocation(wrapped);

                    System.out.println("Object found: " + loc);
                }

                /* TODO Add to a list */
            } catch (IOException e) {
                e.printStackTrace();
                break;
            }
        }

        socket.close();
    }

    /* TODO Convert to a Thread */
    /* TODO Add API to return list of objects */
    /* TODO Make main a wrapper around API */
}
