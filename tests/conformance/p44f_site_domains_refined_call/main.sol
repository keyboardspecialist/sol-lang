module probe
function helper(v: Int64) -> Bool effects { pure } { return v > 0 }
type Positive = refined Int64 where helper(self)
function core(v: Int64) -> Int64 effects { pure } { let checked = Positive(v) return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
