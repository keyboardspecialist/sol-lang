module conformance.p44c_ensures_old_roots

function checked_first(value: Int64) -> Int64 effects { pure }
ensures { result == old(value) + 1 } { return value + 1 }

function checked_second(value: Bool) -> Bool effects { pure }
ensures { result == old(value) } { return value }

@entry
public function first() -> Int64 effects { pure } {
    return checked_first(41)
}

public function second() -> Bool effects { pure } {
    return checked_second(true)
}
