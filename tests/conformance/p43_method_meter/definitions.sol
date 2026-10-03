module conformance.p43_method_meter.definitions

public trait Counter {
    function read(self: borrow Self) -> Int64 effects { pure }
}

implementation Counter for Int64 {
    function read(self: borrow Self) -> Int64 effects { pure } { return self }
}
