module conformance.p44c_refined_import_reject

type Positive = refined Int64 where self > 0

capability Arguments {
    function count() -> Positive
    effects { process.arguments.count<Self> }
}

@entry
public function launch(arguments: capability Arguments) -> Int64
effects { process.arguments.count<arguments> }
{
    let value = arguments.count()
    if value == value { return 42 } else { return 0 }
}
