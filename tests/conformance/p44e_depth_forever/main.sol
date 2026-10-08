module conformance.p44e_depth_forever

function forever() -> Int64 effects { pure } { return forever() }

@entry
public function launch() -> Int64 effects { pure } { return forever() }
