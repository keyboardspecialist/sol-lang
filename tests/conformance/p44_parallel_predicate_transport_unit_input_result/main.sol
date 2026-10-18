module probe
function helper(unit: (), v: Int64) -> Bool effects { pure } { return v > 0 }
function side(v: Int64) -> () effects { pure } { let ignored = v }
function core(unit: (), v: Int64) -> Int64 effects { pure }
requires { helper(unit, v) && { let ignored = side(v) true } } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core((), 4) }
