package com.example;

/**
 * Simple fixture type for JdtBridge integration tests.
 */
public class Widget {
    private int state;

    public Widget() {
        this.state = 0;
    }

    public int compute(int x) {
        return x + 1;
    }

    public int addState(int delta) {
        this.state += delta;
        return this.state;
    }
}
