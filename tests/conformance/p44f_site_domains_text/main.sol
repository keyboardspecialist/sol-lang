module probe
function core(v: Int64) -> Int64 effects { pure } requires { "x" == "x" } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
