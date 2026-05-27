package com.example;

// Java 14+ record. Exercises the regex fallback's record/sealed
// alternation; see JdtBridge::findTypeDefinition.
public record Point(int x, int y) {
    public int sum() {
        return x + y;
    }
}
