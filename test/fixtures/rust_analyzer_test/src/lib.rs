//! Minimal Rust fixture used by RustAnalyzerBridge integration tests.
//!
//! Exposes a handful of items that tests can query via rust-analyzer's
//! workspace/symbol / textDocument/hover / textDocument/references requests.

/// Adds 1 to the input and returns the result.
pub fn compute(x: i32) -> i32 {
    x + 1
}

/// Calls `compute` — provides a reference site for textDocument/references.
pub fn caller(x: i32) -> i32 {
    compute(x) + compute(x)
}

/// A public struct the tests can locate via findTypeDefinition.
pub struct Widget {
    pub width: i32,
    pub height: i32,
}

impl Widget {
    /// Constructs a new Widget.
    pub fn new(width: i32, height: i32) -> Self {
        Self { width, height }
    }

    /// Returns width * height.
    pub fn area(&self) -> i32 {
        self.width * self.height
    }
}

/// An enum type, also used to exercise findTypeDefinition.
pub enum Shape {
    Circle(f32),
    Rectangle(f32, f32),
}
