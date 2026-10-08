module probe
type Positive = refined Int64 where self > 0
function core(v: Int64) -> Int64 effects { pure } requires { Positive(v) == Positive(v) } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
