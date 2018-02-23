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
                ObjectLocation loc = new ObjectLocation();

                System.out.println("Received frame " + frame + " sent at " + timestamp);
                for (int i = 0; i < MAX_OBJECTS_PER_FRAME; i++) {
                    loc.type = ObjectLocation.ObjectTypes.values()[wrapped.getInt()];
                    if (loc.type == ObjectLocation.ObjectTypes.OBJ_NONE)
                        break;
                    loc.x = wrapped.getInt();
                    loc.y = wrapped.getInt();
                    loc.width = wrapped.getInt();
                    loc.height = wrapped.getInt();
                    loc.probability = wrapped.getInt();

                    System.out.println("Object found");
                }

            } catch (IOException e) {
                e.printStackTrace();
                break;
            }
        }

        socket.close();
    }
}
