module conformance.p44c_refined_method_reject

type Positive = refined Int64 where self > 0

trait Readable {
    function read(self: borrow Self) -> Int64 effects { pure }
}

implementation Readable for Positive {
    function read(self: borrow Self) -> Int64 effects { pure } { return 42 }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = Positive(1)
    return value.read()
}
