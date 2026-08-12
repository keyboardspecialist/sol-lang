module example.models

use example.interfaces.Display

implementation Display for Counter {
    function display(self: Self) -> Text effects { pure } {
        return "counter"
    }
}

public function render<T: Display>(value: T) -> Text effects { pure } {
    return value.display()
}
