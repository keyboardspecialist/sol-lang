module probe
function helper(v: Int64) -> Bool effects { pure } { return v > 0 }
function core(v: Int64) -> Int64 effects { pure } requires { { let f = helper true } } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
