module probe
function core(v: Int64) -> Int64 effects { pure } requires { (1,true) == (1,true) } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
