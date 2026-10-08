module probe
function core(v: Int64) -> Int64 effects { pure } ensures { result > 0 } { if v > 0 { return v } else { return 1 } }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
