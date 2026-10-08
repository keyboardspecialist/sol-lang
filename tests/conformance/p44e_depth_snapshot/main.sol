module conformance.p44e_depth_snapshot

function forever(value: Int64) -> Int64 effects { pure } { return forever(0) }

function checked(value: Int64) -> Int64 effects { pure }
ensures { result == old(value) } { return forever(value) }

@entry
public function launch() -> Int64 effects { pure } { return checked(42) }
