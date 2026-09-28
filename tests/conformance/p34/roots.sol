module p34.roots

capability Console {
    function write(value: Text) -> ()
    effects { console.write<Self> }

}

capability Arguments {
    function count() -> Int64
    effects { process.arguments.count<Self> }

    function get(index: Int64) -> Option<Text>
    effects { process.arguments.get<Self> }
}

capability Configuration {
    function read(key: Text) -> Option<Text>
    effects { configuration.read<Self> }
}

capability Derived derives_from source: capability Console {}

/* This declaration is deliberately not a host operation.  The P3.4 test hook
 * checks its P2 recipe directly, without granting or invoking anything. */
function nested_shape(value: Option<Result<Text, Bool>>) -> () effects { pure } {
    let retained = value
}

function relay(source: borrow capability Console) -> ()
effects { console.write<source> }
{
    source.write("roots")
}

function through_two_helpers(source: borrow capability Console) -> ()
effects { console.write<source> }
{
    relay(source)
}

/* The call closure contains a cycle.  The host call remains owned by relay,
 * and its receiver must retain launch.left as its sole root. */
function recursive_helper(source: borrow capability Console, again: Bool) -> ()
effects { console.write<source> }
{
    relay(source)
    if again {
        recursive_helper(source, false)
    } else {
        ()
    }
}

@entry
public function launch(
    left: capability Console,
    right: capability Console,
    secret: capability Console,
    arguments: capability Arguments,
    configuration: capability Configuration,
) -> ()
effects {
    console.write<left>
    process.arguments.count<arguments>
    process.arguments.get<arguments>
    configuration.read<configuration>
}
{
    through_two_helpers(left)
    recursive_helper(left, true)
    arguments.count()
    arguments.get(0)
    configuration.read("p34")
    nested_shape(some(ok("shape")))
    let first = Derived { source = left }
    let second = Derived { source = right }
    let hidden = Derived { source = secret }
}
