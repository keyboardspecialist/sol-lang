module p34_five.lookalike

capability Console { function write(value: Text) -> () effects { console.write<Self> } }
capability Arguments {
    function count() -> Int64 effects { process.arguments.count<Self> }
    function get(index: Int64) -> Option<Text> effects { process.arguments.get<Self> }
}
capability Configuration { function read(key: Text) -> Option<Text> effects { configuration.read<Self> } }
/* Same call spelling, effect, argument, and result as the approved row, but a
 * distinct capability identity.  It is a real fifth host requirement. */
capability ConsoleLookalike { function write(value: Text) -> () effects { console.write<Self> } }

@entry
public function launch(console: capability Console, arguments: capability Arguments,
    configuration: capability Configuration, lookalike: capability ConsoleLookalike) -> ()
effects { console.write<console> process.arguments.count<arguments>
    process.arguments.get<arguments> configuration.read<configuration>
    console.write<lookalike> }
{
    console.write("approved")
    arguments.count()
    arguments.get(0)
    configuration.read("p34")
    lookalike.write("forged")
}
