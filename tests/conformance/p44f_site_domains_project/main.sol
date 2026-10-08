module probe
record Box { value: Int64 }
function core(v: Int64) -> Int64 effects { pure } requires { { let b = Box { value = v } b.value > 0 } } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
