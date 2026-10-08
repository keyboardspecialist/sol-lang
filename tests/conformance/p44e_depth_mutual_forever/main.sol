module conformance.p44e_depth_mutual_forever

function left() -> Int64 effects { pure } { return right() }
function right() -> Int64 effects { pure } { return left() }

@entry
public function launch() -> Int64 effects { pure } { return left() }
