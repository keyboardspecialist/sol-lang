module conformance.p44e_depth_packet

function bottom() -> Int64 effects { pure } { return bottom() }
function middle() -> Int64 effects { pure } { return bottom() }

@entry
public function launch() -> Int64 effects { pure } { return middle() }
