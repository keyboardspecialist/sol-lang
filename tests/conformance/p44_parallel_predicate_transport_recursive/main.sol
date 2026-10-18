module probe
function helper(v: Int64) -> Bool effects { pure } {
    if v > 0 { return helper(v - 1) } else { return true }
}
function core(v: Int64) -> Int64 effects { pure } requires { helper(v) } { return v }
@entry
public function launch() -> Int64 effects { pure } { return core(4) }
