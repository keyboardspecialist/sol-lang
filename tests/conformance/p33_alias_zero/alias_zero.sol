module runtime_alias_zero
function callback(value: Int64) -> Bool effects { pure } { return true }
function root() -> Bool effects { pure } requires { { let exact = callback exact(1) } } { return callback(1) }
