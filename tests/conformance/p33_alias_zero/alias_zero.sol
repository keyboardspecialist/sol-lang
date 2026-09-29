module runtime_bound_environment
capability Base { function choose(value: Int64) -> Bool effects { pure } }
function callback(value: Int64) -> Bool effects { pure } { return true }
function root(base: capability Base) -> Bool effects { pure } requires { { let exact = callback let first = base.choose let second = base.choose exact(1) && first(1) && second(2) } } { return callback(1) && base.choose(1) }
