module conformance.p44e_depth_method_inout

trait Counter {
    function bump(self: inout Self) -> () effects { pure }
}

implementation Counter for Int64 {
    function bump(self: inout Self) -> () effects { pure } {
        if self > 0 {
            var next = self - 1
            next.bump()
            self = next
        } else { () }
    }
}

@entry
public function launch() -> Int64 effects { pure } {
    var value = 62
    value.bump()
    return value + 42
}
public function fail() -> Int64 effects { pure } { return 43 }
