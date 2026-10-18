module probe
function leaf(v: Int64) -> Int64 effects { pure } { return v }
function helper(v: Int64) -> Int64 effects { pure }
requires { v > 0 } ensures { result == old(v) } { return leaf(v) }
function core(v: Int64) -> Int64 effects { pure }
ensures { helper(result) == old(v) } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
