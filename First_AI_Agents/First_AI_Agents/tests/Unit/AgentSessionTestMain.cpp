#include <exception>
#include <iostream>

int runOpenAIAgentSessionTests();
int runAnthropicAgentSessionTests();
int runStrictSchemaContractTests();

int main()
{
    try {
        runOpenAIAgentSessionTests();
        runAnthropicAgentSessionTests();
        runStrictSchemaContractTests();
        std::cout << "AgentSession tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "AgentSession tests failed: " << e.what() << '\n';
        return 1;
    }
}
