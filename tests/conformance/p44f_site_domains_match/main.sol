module probe
function core(v: Int64) -> Int64 effects { pure } requires { match true { true => true false => false } } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
