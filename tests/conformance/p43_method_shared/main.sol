module conformance.p43_method_shared

trait Counter {
    function read(self: borrow Self) -> Int64 effects { pure }
}

implementation Counter for Int64 {
    function read(self: borrow Self) -> Int64 effects { pure } { return self }
}

@entry
public function launch() -> Int64 effects { pure } {
    var value = 41
    return value.read()
}
