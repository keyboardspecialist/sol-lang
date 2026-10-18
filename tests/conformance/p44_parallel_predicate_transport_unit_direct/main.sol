module probe
function helper(v: ()) -> () effects { pure } { return v }
function core(v: Int64) -> Int64 effects { pure } requires { helper(()) == () } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
