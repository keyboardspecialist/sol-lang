module probe
function consume(v: ()) -> () effects { pure } { return v }
function helper(flag: Bool) -> Bool effects { pure } { let unit = consume(()) return flag }
function core(v: Int64) -> Int64 effects { pure } requires { helper(true) } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
