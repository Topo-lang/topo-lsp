package com.example;

/**
 * Entry-point fixture that references Widget.compute so JDT's workspace index
 * sees at least one cross-file reference.
 */
public class App {
    public static void main(String[] args) {
        Widget widget = new Widget();
        int result = widget.compute(41);
        widget.addState(result);
        System.out.println(result);
    }
}
