




module conformance.p36_graph

capability Provider {
    function choose(value: Int64) -> Bool effects { pure } { return value > 0 }
}

function launch(provider: capability Provider) -> Bool effects { pure }
requires { { let bound = provider.choose bound(1) } }
{
    return provider.choose(1)
}
