package org.mayheminc;

import java.nio.*;

public class ObjectLocation {
    public enum ObjectTypes {
        OBJ_NONE,
        OBJ_CUBE,
        OBJ_SCALE_CENTER,
        OBJ_SCALE_BLUE,
        OBJ_SCALE_RED,
        OBJ_SWITCH_RED,
        OBJ_SWITCH_BLUE,
        OBJ_PORTAL_RED,
        OBJ_PORTAL_BLUE,
        OBJ_EXCHANGE_RED,
        OBJ_EXCHANGE_BLUE,
        OBJ_BUMPERS_RED,
        OBJ_BUMPERS_BLUE,
        OBJ_BUMPERS_CLASS13,
        OBJ_BUMPERS_CLASS14,
        OBJ_BUMPERS_CLASS15,
        OBJ_BUMPERS_CLASS16,
        OBJ_BUMPERS_CLASS17,
        OBJ_BUMPERS_CLASS18,
        OBJ_BUMPERS_CLASS19,
        OBJ_BUMPERS_CLASS20,
        OBJ_EOL,
    };

    public ObjectTypes type;
    public float x;
    public float y;
    public float width;
    public float height;
    public float probability;

    public ObjectLocation() {
        type = ObjectTypes.OBJ_NONE;
        x = y = width = height = probability = 0;
    }
}
