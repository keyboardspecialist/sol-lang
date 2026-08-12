module example.rules

public function nonnegative(value: Int64) -> Bool effects { pure } {
    return value >= 0
}
