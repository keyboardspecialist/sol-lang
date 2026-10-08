module conformance.p44e_depth_method

trait Counter {
    function read(self: borrow Self) -> Int64 effects { pure }
    function bump(self: inout Self) -> () effects { pure }
}

implementation Counter for Int64 {
    function read(self: borrow Self) -> Int64 effects { pure } {
        if self == 0 { return 42 } else {
            let next = self - 1
            return next.read()
        }
    }
    function bump(self: inout Self) -> () effects { pure } {
        if self > 0 {
            self -= 1
            self.bump()
        } else { () }
    }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = 62
    return value.read()
}
