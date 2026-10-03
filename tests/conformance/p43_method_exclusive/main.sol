module conformance.p43_method_exclusive

trait Counter {
    function bump(self: inout Self) -> () effects { pure }
}

implementation Counter for Int64 {
    function bump(self: inout Self) -> () effects { pure } { self += 1 }
}

@entry
public function launch() -> Int64 effects { pure } {
    var value = 41
    value.bump()
    return value
}
