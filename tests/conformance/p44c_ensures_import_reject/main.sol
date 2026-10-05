module conformance.p44c_ensures_import_reject

capability Arguments {
    function count() -> Int64
    effects { process.arguments.count<Self> }
    ensures { result >= 0 }
}

@entry
public function launch(arguments: capability Arguments) -> Int64
effects { process.arguments.count<arguments> }
{
    return arguments.count()
}
