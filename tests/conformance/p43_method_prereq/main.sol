module conformance.p43_method_prereq

trait Counter {
    function read(self: borrow Self) -> Int64 effects { pure }
    function bump(self: inout Self) -> () effects { pure }
}

implementation Counter for Int64 {
    function read(self: borrow Self) -> Int64 effects { pure } { return self }
    function bump(self: inout Self) -> () effects { pure } { self += 1 }
}

@entry
public function launch() -> Int64 effects { pure } {
    var value = 41
    let observed = value.read()
    value.bump()
    return observed + value
}

public function fail() -> Int64 effects { pure } {
    var value = 9223372036854775807
    value.bump()
    return value
}
