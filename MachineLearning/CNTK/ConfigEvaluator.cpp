// ConfigEvaluator.cpp -- execute what's given in a config file

// main TODO items:
//  - dictionary merging, to allow overwriting from command line
//     - [ d1 ] + [ d2 ] will install a filter in d1 to first check against d2
//     - d2 can have fully qualified names on the LHS, and the filter is part of a chain that is passed down to inner dictionaries created
//     - d1 + d2 == wrapper around d1 with filter(d2)
//       When processing [ ] expressions inside d1, the current filter chain is applied straight away.
//     - model merging =
//        - Network exposes dictionary          // or use explicit expression new ConfigRecord(network)?
//        - ^^ + [ new nodes ] - [ nodes to delete ]
//          creates modified network
//        - pass into new NDLComputationNetwork
//  - fix the problem that ConfigValuePtrs are not really copyable (do this by move semantics instead of copying)
//  - I get stack overflows...? What's wrong with stack usage?? Need to use more references? Or only a problem in Debug?
//  - a way to access a symbol up from the current scope, needed for function parameters of the same name as dict entries created from them, e.g. the optional 'tag'
//     - ..X (e.g. ..tag)? Makes semi-sense, but syntactically easy, and hopefully not used too often
//     - or MACRO.X (e.g. Parameter.tag); latter would require to reference macros by name as a clearly defined mechanism, but hard to implement (ambiguity)
//  - config[".."] should search symbols the entire stack up, not only the current dictionary
//  - name lookup should inject TextLocation into error stack

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "Basics.h"
#include "ConfigEvaluator.h"
#include <deque>
#include <set>
#include <functional>
#include <memory>
#include <cmath>

#ifndef let
#define let const auto
#endif

namespace Microsoft { namespace MSR { namespace CNTK { class ComputationNetwork; }}}

namespace Microsoft { namespace MSR { namespace CNTK { namespace Config {

    using namespace std;
    using namespace msra::strfun;

    bool trace = false;// true;      // enable to get debug output

#define exprPathSeparator L"."

    // =======================================================================
    // string formatting
    // =======================================================================

    wstring IndentString(wstring s, size_t indent)
    {
        const wstring prefix(indent, L' ');
        size_t pos = 0;
        for (;;)
        {
            s.insert(pos, prefix);
            pos = s.find(L'\n', pos + 2);
            if (pos == wstring::npos)
                return s;
            pos++;
        }
    }
    wstring NestString(wstring s, wchar_t open, bool newline, wchar_t close)
    {
        wstring result = IndentString(s, 2);
        if (newline)        // have a new line after the open symbol
            result = L" \n" + result + L"\n ";
        else
            result.append(L"  ");
        result.front() = open;
        result.back() = close;
        return result;
    }

    // 'how' is the center of a printf format string, without % and type. Example %.2f -> how=".2"
    // TODO: change to taking a regular format string and a :: array of args that are checked. Support d,e,f,g,x,c,s (s also for ToString()).
    // TODO: :: array. Check if that is the right operator for e.g. Haskell.
    // TODO: turn Print into PrintF; e.g. PrintF provides 'format' arg. Printf('solution to %s is %d', 'question' :: 42)
    static wstring FormatConfigValue(ConfigValuePtr arg, const wstring & how)
    {
        size_t pos = how.find(L'%');
        if (pos != wstring::npos)
            RuntimeError("FormatConfigValue: format string must not contain %");
        if (arg.Is<String>())
        {
            return wstrprintf((L"%" + how + L"s").c_str(), arg.AsRef<String>().c_str());
        }
        else if (arg.Is<Double>())
        {
            let val = arg.AsRef<Double>();
            if (val == (int)val)
                return wstrprintf((L"%" + how + L"d").c_str(), (int)val);
            else
                return wstrprintf((L"%" + how + L"f").c_str(), val);
        }
        else if (arg.Is<ConfigRecord>())
        {
            let record = arg.AsPtr<ConfigRecord>();
            let members = record->GetMembers();
            wstring result;
            bool first = true;
            for (auto iter : members)
            {
                if (first)
                    first = false;
                else
                    result.append(L"\n");
                result.append(iter.first);
                result.append(L" = ");
                result.append(FormatConfigValue(iter.second, how));
            }
            return NestString(result, L'[', true, L']');
        }
        else if (arg.Is<ConfigArray>())
        {
            let arr = arg.AsPtr<ConfigArray>();
            wstring result;
            let range = arr->GetRange();
            for (int i = range.first; i <= range.second; i++)
            {
                if (i > range.first)
                    result.append(L"\n");
                result.append(FormatConfigValue(arr->At(i, TextLocation()), how));
            }
            return NestString(result, L'(', false, L')');
        }
        else if (arg.Is<HasToString>())
            return arg.AsRef<HasToString>().ToString();
        else
            return msra::strfun::utf16(arg.TypeName());             // cannot print this type
    }

    // =======================================================================
    // dummy implementation of several ComputationNode derivates for experimental purposes
    // =======================================================================

    struct Matrix { size_t rows; size_t cols; Matrix(size_t rows, size_t cols) : rows(rows), cols(cols) { } };
    typedef shared_ptr<Matrix> MatrixPtr;

    // a ComputationNode that derives from MustFinalizeInit does not resolve some args immediately (just keeps ConfigValuePtrs),
    // assuming they are not ready during construction.
    // This is specifically meant to be used by DelayNode, see comments there.
    struct MustFinalizeInit { virtual void FinalizeInit() = 0; };   // derive from this to indicate ComputationNetwork should call FinalizeIitlate initialization

    // TODO: implement ConfigRecord should this expose a config dict to query the dimension (or only InputValues?)? Expose Children too? As list and by name?
    struct ComputationNode : public Object, public HasToString, public HasName
    {
        typedef shared_ptr<ComputationNode> ComputationNodePtr;

        // inputs and output
        vector<ComputationNodePtr> m_children;  // these are the inputs
        MatrixPtr m_functionValue;              // this is the result

        // other
        wstring m_nodeName;                     // node name in the graph
        static wstring TidyName(wstring name)
        {
#if 0
            // clean out the intermediate name, e.g. A._b.C -> A.C for pretty printing of names, towards dictionary access
            // BUGBUG: anonymous ComputationNodes will get a non-unique name this way
            if (!name.empty())
            {
                let pos = name.find(exprPathSeparator);
                let left = pos == wstring::npos ? name : name.substr(0, pos);
                let right = pos == wstring::npos ? L"" : TidyName(name.substr(pos + 1));
                if (left.empty() || left[0] == '_')
                    name = right;
                else if (right.empty())
                    name = left;
                else
                    name = left + exprPathSeparator + right;
            }
#endif
            return name;
        }
        wstring NodeName() const { return m_nodeName; }        // TODO: should really be named GetNodeName()
        /*HasName::*/ void SetName(const wstring & name) { m_nodeName = name; }

        wstring m_tag;
        void SetTag(const wstring & tag) { m_tag = tag; }
        const wstring & GetTag() const { return m_tag; }

        virtual const wchar_t * OperationName() const = 0;

        ComputationNode()
        {
            // node nmaes are not implemented yet; use a unique node name instead
            static int nodeIndex = 1;
            m_nodeName = wstrprintf(L"anonymousNode%d", nodeIndex);
            nodeIndex++;
        }

        virtual void AttachInputs(ComputationNodePtr arg)
        {
            m_children.resize(1);
            m_children[0] = arg;
        }
        virtual void AttachInputs(ComputationNodePtr leftNode, ComputationNodePtr rightNode)
        {
            m_children.resize(2);
            m_children[0] = leftNode;
            m_children[1] = rightNode;
        }
        virtual void AttachInputs(ComputationNodePtr arg1, ComputationNodePtr arg2, ComputationNodePtr arg3)
        {
            m_children.resize(3);
            m_children[0] = arg1;
            m_children[1] = arg2;
            m_children[2] = arg3;
        }
        void AttachInputs(vector<ComputationNodePtr> && inputs, size_t num = 0/*0 means all OK*/)
        {
            if (num != 0 && inputs.size() != num)
                LogicError("AttachInputs: called with incorrect number of arguments");
            m_children = inputs;
        }
        const std::vector<ComputationNodePtr> & GetChildren() const { return m_children; }

        /*HasToString::*/ wstring ToString() const
        {
            // we format it like "[TYPE] ( args )"
            wstring result = TidyName(NodeName()) + L" : " + wstring(OperationName());
            if (m_children.empty()) result.append(L"()");
            else
            {
                wstring args;
                bool first = true;
                for (auto & child : m_children)
                {
                    if (first)
                        first = false;
                    else
                        args.append(L"\n");
                    args.append(TidyName(child->NodeName()));
                }
                result += L" " + NestString(args, L'(', true, ')');
            }
            return result;
        }
    };
    typedef ComputationNode::ComputationNodePtr ComputationNodePtr;
    struct UnaryComputationNode : public ComputationNode
    {
        UnaryComputationNode(vector<ComputationNodePtr> && inputs, const wstring & tag) { AttachInputs(move(inputs), 1); SetTag(tag); }
    };
    struct BinaryComputationNode : public ComputationNode
    {
        BinaryComputationNode(vector<ComputationNodePtr> && inputs, const wstring & tag) { AttachInputs(move(inputs), 2); SetTag(tag); }
    };
    struct TernaryComputationNode : public ComputationNode
    {
        TernaryComputationNode(vector<ComputationNodePtr> && inputs, const wstring & tag) { AttachInputs(move(inputs), 3); SetTag(tag); }
    };

#define DefineComputationNode(T,C) \
    struct T##Node : public C##ComputationNode \
    { \
    T##Node(vector<ComputationNodePtr> && inputs, const wstring & tag) : C##ComputationNode(move(inputs), tag) { } \
    /*ComputationNode::*/ const wchar_t * OperationName() const { return L#T; } \
    };
#define DefineUnaryComputationNode(T)   DefineComputationNode(T,Unary)
#define DefineBinaryComputationNode(T)  DefineComputationNode(T,Binary)
#define DefineTernaryComputationNode(T) DefineComputationNode(T,Ternary)
    DefineBinaryComputationNode(Plus);
    DefineBinaryComputationNode(Minus);
    DefineBinaryComputationNode(Times);
    DefineBinaryComputationNode(DiagTimes);
    DefineBinaryComputationNode(Scale);
    DefineUnaryComputationNode(Log);
    DefineUnaryComputationNode(Sigmoid);
    DefineUnaryComputationNode(Mean);
    DefineUnaryComputationNode(InvStdDev);
    DefineTernaryComputationNode(PerDimMeanVarNormalization);
    DefineBinaryComputationNode(CrossEntropyWithSoftmax);
    DefineBinaryComputationNode(ErrorPrediction);

#if 0   // ScaleNode is something more complex it seems
    class ScaleNode : public ComputationNode
    {
        double factor;
    public:
        PlusNode(vector<ComputationNodePtr> && inputs, const wstring & tag) : BinaryComputationNode(move(inputs), tag) { }
        /*implement*/ const wchar_t * OperationName() const { return L"Scale"; }
    };
#endif
    struct RowSliceNode : public UnaryComputationNode
    {
        size_t firstRow, numRows;
    public:
        RowSliceNode(vector<ComputationNodePtr> && inputs, size_t firstRow, size_t numRows, const wstring & tag) : UnaryComputationNode(move(inputs), tag), firstRow(firstRow), numRows(numRows) { }
        /*ComputationNode::*/ const wchar_t * OperationName() const { return L"RowSlice"; }
    };
    // DelayNode is special in that it may for cycles.
    // Specifically, to break circular references, DelayNode does not resolve its input arg (a ComputationNode), but rather keeps the ConfigValuePtr for now.
    // The ConfigValuePtr is meant to be unresolved, i.e. a lambda that will resolve its arg when accessing the value for the first time.
    // I.e. after construction, DelayNode can be referenced, but it cannot perform any operation on its argument, since it does not know it yet.
    // ComputationNetwork knows to call FinalizeInit() to resolve this, at a time when pointers for anythin this may reference
    // from its or outer scope have been created (if those pointers are to Delay nodes in turn, those would again resolve in their
    // later FinalizeInit() call, which may yet again create new nodes etc.).
    struct DelayNode : public ComputationNode, public MustFinalizeInit
    {
        ConfigValuePtr argUnresolved;
        ComputationNodePtr arg;
        int deltaT;
    public:
        DelayNode(ConfigValuePtr argUnresolved, int deltaT, const wstring & tag) : argUnresolved(argUnresolved), deltaT(deltaT) { SetTag(tag); }
        /*MustFinalizeInit::*/ void FinalizeInit()
        {
            AttachInputs(vector<ComputationNodePtr>(1,argUnresolved));             // the implied type cast resolves it
            argUnresolved = ConfigValuePtr();       // and free any references it may hold
            // dim?
        }
        /*ComputationNode::*/ const wchar_t * OperationName() const { return L"Delay"; }
    };
    class InputValue : public ComputationNode
    {
    public:
        InputValue(const ConfigRecord & config) // TODO
        {
            config;
        }
        /*ComputationNode::*/ const wchar_t * OperationName() const { return L"InputValue"; }
    };
    class LearnableParameter : public ComputationNode
    {
        size_t outDim, inDim;
    public:
        LearnableParameter(size_t outDim, size_t inDim) : outDim(outDim), inDim(inDim) { }
        /*ComputationNode::*/ const wchar_t * OperationName() const { return L"LearnableParameter"; }
        /*HasToString::*/ wstring ToString() const
        {
            return wstrprintf(L"%ls : %ls (%d, %d)", TidyName(NodeName()).c_str(), OperationName(), (int)outDim, (int)inDim);
        }
    };
    // helper for the factory function for ComputationNodes
    static vector<ComputationNodePtr> GetInputs(const ConfigRecord & config, size_t expectedNumInputs, const wstring & classId/*for error msg*/)
    {
        vector<ComputationNodePtr> inputs;
        let inputsArg = config[L"inputs"];
        if (inputsArg.Is<ComputationNode>())  // single arg
            inputs.push_back(inputsArg);
        else
        {
            let inputsArray = (ConfigArrayPtr)inputsArg;
            let range = inputsArray->GetRange();
            for (int i = range.first; i <= range.second; i++)
                inputs.push_back(inputsArray->At(i, inputsArg.GetLocation()));
        }
        if (inputs.size() != expectedNumInputs)
            throw EvaluationError(L"unexpected number of inputs to ComputationNode class " + classId, inputsArg.GetLocation());
        return inputs;
    }
    // factory function for ComputationNodes
    template<>
    shared_ptr<ComputationNode> MakeRuntimeObject<ComputationNode>(const ConfigRecord & config)
    {
        let classIdParam = config[L"class"];
        wstring classId = classIdParam;
        let tagp = config.Find(L"optionalTag");
        wstring tag = tagp ? *tagp : wstring();
        if (classId == L"LearnableParameterNode")
            return make_shared<LearnableParameter>(config[L"outDim"], config[L"inDim"]);
        else if (classId == L"PlusNode")
            return make_shared<PlusNode>(GetInputs(config, 2, L"PlusNode"), tag);
        else if (classId == L"MinusNode")
            return make_shared<MinusNode>(GetInputs(config, 2, L"MinusNode"), tag);
        else if (classId == L"TimesNode")
            return make_shared<TimesNode>(GetInputs(config, 2, L"TimesNode"), tag);
        else if (classId == L"DiagTimesNode")
            return make_shared<DiagTimesNode>(GetInputs(config, 2, L"DiagTimesNode"), tag);
        // BUGBUG: ScaleNode is given a BoxOf<Double>, not ComputationNode
        else if (classId == L"ScaleNode")
            return make_shared<ScaleNode>(GetInputs(config, 2, L"ScaleNode"), tag);
        else if (classId == L"LogNode")
            return make_shared<LogNode>(GetInputs(config, 1, L"LogNode"), tag);
        else if (classId == L"SigmoidNode")
            return make_shared<SigmoidNode>(GetInputs(config, 1, L"SigmoidNode"), tag);
        else if (classId == L"MeanNode")
            return make_shared<MeanNode>(GetInputs(config, 1, L"MeanNode"), tag);
        else if (classId == L"InvStdDevNode")
            return make_shared<InvStdDevNode>(GetInputs(config, 1, L"InvStdDevNode"), tag);
        else if (classId == L"PerDimMeanVarNormalizationNode")
            return make_shared<PerDimMeanVarNormalizationNode>(GetInputs(config, 3, L"PerDimMeanVarNormalizationNode"), tag);
        else if (classId == L"RowSliceNode")
            return make_shared<RowSliceNode>(GetInputs(config, 1, L"RowSliceNode"), (size_t)config[L"first"], (size_t)config[L"num"], tag);
        else if (classId == L"CrossEntropyWithSoftmaxNode")
            return make_shared<CrossEntropyWithSoftmaxNode>(GetInputs(config, 2, L"CrossEntropyWithSoftmaxNode"), tag);
        else if (classId == L"ErrorPredictionNode")
            return make_shared<ErrorPredictionNode>(GetInputs(config, 2, L"ErrorPredictionNode"), tag);
        else if (classId == L"DelayNode")
            return make_shared<DelayNode>(config[L"input"], config[L"deltaT"], tag);
        else
            throw EvaluationError(L"unknown ComputationNode class " + classId, classIdParam.GetLocation());
    }

    // =======================================================================
    // dummy implementations of ComputationNetwork derivates
    // =======================================================================

    // ComputationNetwork class
    class ComputationNetwork : public Object, public IsConfigRecord
    {
    protected:
        map<wstring, ComputationNodePtr> m_namesToNodeMap;      // root nodes in this network; that is, nodes defined in the dictionary
    public:
        // pretending to be a ConfigRecord
        /*IsConfigRecord::*/ const ConfigValuePtr & operator()(const wstring & id, wstring message) const   // e.g. confRec(L"message", helpString)
        {
            id; message; RuntimeError("unknown class parameter");    // (for now)
        }
        /*IsConfigRecord::*/ const ConfigValuePtr * Find(const wstring & id) const         // returns nullptr if not found
        {
            id; return nullptr; // (for now)
        }
    };

    class NDLComputationNetwork : public ComputationNetwork, public HasToString
    {
        set<ComputationNodePtr> inputs;     // all input nodes
        set<ComputationNodePtr> outputs;    // all output nodes
        set<ComputationNodePtr> parameters; // all parameter nodes
    public:
        NDLComputationNetwork(const ConfigRecord & config)
        {
            deque<ComputationNodePtr> workList;
            // flatten the set of all nodes
            // we collect all ComputationNodes from the config; that's it
            for (auto & iter : config.GetMembers())
                if (iter.second.Is<ComputationNode>())
                    workList.push_back((ComputationNodePtr)config[iter.first]);
            // process work list
            // Also call FinalizeInit where we must.
            set<ComputationNodePtr> allChildren;    // all nodes that are children of others (those that are not are output nodes)
            while (!workList.empty())
            {
                let n = workList.front();
                workList.pop_front();
                // add to set
                let res = m_namesToNodeMap.insert(make_pair(n->NodeName(), n));
                if (!res.second)        // not inserted: we already got this one
                if (res.first->second != n)
                    LogicError("NDLComputationNetwork: multiple nodes with the same NodeName()");
                else
                    continue;
                // If node derives from MustFinalizeInit() then it has unresolved ConfigValuePtrs. Resolve them now.
                // This may generate a whole new load of nodes, including nodes which in turn have late init.
                // TODO: think this through whether it may generate delays nevertheless
                let mustFinalizeInit = dynamic_pointer_cast<MustFinalizeInit>(n);
                if (mustFinalizeInit)
                    mustFinalizeInit->FinalizeInit();
                // TODO: ...can we do stuff like propagating dimensions here? Or still too early?
                // get children
                // traverse children (i.e., append them to the work list)
                let children = n->GetChildren();
                for (auto c : children)
                {
                    workList.push_back(c);  // (we could check whether c is in 'nodes' here to optimize, but this way it is cleaner)
                    allChildren.insert(c);  // also keep track of all children, for computing the 'outputs' set below
                }
            }
            // build sets of special nodes
            for (auto iter : m_namesToNodeMap)
            {
                let n = iter.second;
                if (n->GetChildren().empty())
                {
                    if (dynamic_pointer_cast<InputValue>(n))
                        inputs.insert(n);
                    else if (dynamic_pointer_cast<LearnableParameter>(n))
                        parameters.insert(n);
                    else
                        LogicError("ComputationNetwork: found child-less node that is neither InputValue nor LearnableParameter");
                }
                if (allChildren.find(n) == allChildren.end())
                    outputs.insert(n);
            }
            m_namesToNodeMap;
        }
        /*HasToString::*/ wstring ToString() const
        {
            wstring args;
            bool first = true;
            for (auto & iter : m_namesToNodeMap)
            {
                let node = iter.second;
                if (first)
                    first = false;
                else
                    args.append(L"\n");
                args.append(node->ToString());
            }
            return L"NDLComputationNetwork " + NestString(args, L'[', true, ']');
        }
    };

    // =======================================================================
    // built-in functions (implemented as Objects that are also their value)
    // =======================================================================

    // StringFunction implements
    //  - Format
    //  - Chr(c) -- gives a string of one character with Unicode value 'c'
    //  - Replace(s,what,withwhat) -- replace all occurences of 'what' with 'withwhat'
    //  - Substr(s,begin,num) -- get a substring
    // TODO: RegexReplace()     Substr takes negative position to index from end, and length -1
    class StringFunction : public String
    {
        wstring Replace(wstring s, const wstring & what, const wstring & withwhat)
        {
            wstring res = s;
            auto pos = res.find(what);
            while (pos != wstring::npos)
            {
                res = res.substr(0, pos) + withwhat + res.substr(pos + what.size());
                pos = res.find(what, pos + withwhat.size());
            }
            return res;
        }
        wstring Substr(const wstring & s, int ibegin, int inum)
        {
            // negative index indexes from end; index may exceed
            let begin = min(ibegin < 0 ? s.size() + ibegin : ibegin, s.size());
            // 'num' is allowed to exceed
            let num = min(inum < 0 ? SIZE_MAX : inum, s.size() - begin);
            return s.substr(begin, num);
        }
    public:
        StringFunction(const ConfigRecord & config)
        {
            wstring & us = *this;   // we write to this
            let arg = config[L"arg"];
            let whatArg = config[L"what"];
            wstring what = whatArg;
            if (what == L"Format")
                us = FormatConfigValue(arg, config[L"how"]);
            else if (what == L"Chr")
                us = wstring(1, (wchar_t)(double)arg);
            else if (what == L"Substr")
                us = Substr(arg, config[L"pos"], config[L"chars"]);
            else if (what == L"Replace")
                us = Replace(arg, config[L"replacewhat"], config[L"withwhat"]);
            else
                throw EvaluationError(L"unknown 'what' value to StringFunction: " + what, whatArg.GetLocation());
        }
    };

    // NumericFunctions
    //  - Floor()
    //  - Length() (of string or array)
    class NumericFunction : public BoxOf<Double>
    {
    public:
        NumericFunction(const ConfigRecord & config) : BoxOf<Double>(0.0)
        {
            double & us = *this;   // we write to this
            let arg = config[L"arg"];
            let whatArg = config[L"what"];
            wstring what = whatArg;
            if (what == L"Floor")
                us = floor((double)arg);
            else if (what == L"Length")
            {
                if (arg.Is<String>())
                    us = (double)((wstring)arg).size();
                else        // otherwise expect an array
                {
                    let arr = (ConfigArray)arg;
                    let range = arr.GetRange();
                    us = (double)(range.second + 1 - range.first);
                }
            }
            else
                throw EvaluationError(L"unknown 'what' value to NumericFunction: " + what, whatArg.GetLocation());
        }
    };

    // =======================================================================
    // general-purpose use Actions
    // =======================================================================

    // sample runtime objects for testing
    // We are trying all sorts of traits here, even if they make no sense for PrintAction.
    class PrintAction : public Object, public HasName
    {
    public:
        PrintAction(const ConfigRecord & config)
        {
            let what = config(L"what", L"This specifies the object to print.");
            let str = what.Is<String>() ? what : FormatConfigValue(what, L""); // convert to string (without formatting information)
            fprintf(stderr, "%ls\n", str.c_str());
        }
        /*HasName::*/ void SetName(const wstring & name)
        {
            name;
        }
    };

    class AnotherAction : public Object
    {
    public:
        AnotherAction(const ConfigRecord &) { fprintf(stderr, "Another\n"); }
        virtual ~AnotherAction(){}
    };

    // FailAction just throw a config error
    class FailAction : public Object
    {
    public:
        FailAction(const ConfigRecord & config)
        {
            // note: not quite optimal yet in terms of how the error is shown; e.g. ^ not showing under offending variable
            wstring message = config[L"what"];
            bool fail = true;
            if (fail)   // this will trick the VS compiler into not issuing warning 4702: unreachable code
                throw EvaluationError(message, TextLocation()/*no location means it will show the parent's location*/);
        }
    };

    shared_ptr<Object> MakeExperimentalComputationNetwork(const ConfigRecord &);
    shared_ptr<Object> MakeExperimentalComputationNode(const ConfigRecord &);

    // =======================================================================
    // Evaluator -- class for evaluating a syntactic parse tree
    // Evaluation converts a parse tree from ParseConfigString/File() into a graph of live C++ objects.
    // =======================================================================

    // -----------------------------------------------------------------------
    // error handling
    // -----------------------------------------------------------------------

    __declspec(noreturn) static void Fail(const wstring & msg, TextLocation where) { throw EvaluationError(msg, where); }
    __declspec(noreturn) static void TypeExpected(const wstring & what, ExpressionPtr e) { Fail(L"expected expression of type " + what, e->location); }
    __declspec(noreturn) static void UnknownIdentifier(const wstring & id, TextLocation where) { Fail(L"unknown identifier " + id, where); }

    // -----------------------------------------------------------------------
    // access to ConfigValuePtr content with error messages
    // -----------------------------------------------------------------------

    // get value
    template<typename T>
    static shared_ptr<T> AsPtr(ConfigValuePtr value, ExpressionPtr e, const wchar_t * typeForMessage)
    {
        if (!value.Is<T>())
            TypeExpected(typeForMessage, e);
        return value.AsPtr<T>();
    }

    static double ToDouble(ConfigValuePtr value, ExpressionPtr e)
    {
        let val = dynamic_cast<Double*>(value.get());
        if (!val)
            TypeExpected(L"number", e);
        double & dval = *val;
        return dval;    // great place to set breakpoint
    }

    // get number and return it as an integer (fail if it is fractional)
    static int ToInt(ConfigValuePtr value, ExpressionPtr e)
    {
        let val = ToDouble(value, e);
        let res = (int)(val);
        if (val != res)
            TypeExpected(L"integer", e);
        return res;
    }

    static bool ToBoolean(ConfigValuePtr value, ExpressionPtr e)
    {
        let val = dynamic_cast<Bool*>(value.get());            // TODO: factor out this expression
        if (!val)
            TypeExpected(L"boolean", e);
        return *val;
    }

    // -----------------------------------------------------------------------
    // configurable runtime types ("new" expression)
    // -----------------------------------------------------------------------

    // helper for configurableRuntimeTypes initializer below
    // This returns a ConfigurableRuntimeType info structure that consists of
    //  - a lambda that is a constructor for a given runtime type and
    //  - a bool saying whether T derives from IsConfigRecord
    struct ConfigurableRuntimeType
    {
        bool isConfigRecord;
        function<ConfigValuePtr(const ConfigRecord &, TextLocation, const wstring &)> construct; // lambda to construct an object of this class
    };

    template<class C>
    static ConfigurableRuntimeType MakeRuntimeTypeConstructor()
    {
        ConfigurableRuntimeType info;
        info.construct = [](const ConfigRecord & config, TextLocation location, const wstring & exprPath) // lambda to construct
        {
            return ConfigValuePtr(MakeRuntimeObject<C>(config), location, exprPath);
        };
        info.isConfigRecord = is_base_of<IsConfigRecord, C>::value;
        return info;
    }
#if 0
    static ConfigurableRuntimeType MakeExperimentalComputationNetworkConstructor()
    {
        ConfigurableRuntimeType info;
        info.construct = [](const ConfigRecord & config, TextLocation location, const wstring & exprPath) // lambda to construct
        {
            return ConfigValuePtr(MakeExperimentalComputationNetwork(config), location, exprPath);
        };
        info.isConfigRecord = true;
        return info;
    }
    static ConfigurableRuntimeType MakeExperimentalComputationNodeConstructor()
    {
        ConfigurableRuntimeType info;
        info.construct = [](const ConfigRecord & config, TextLocation location, const wstring & exprPath) // lambda to construct
        {
            return ConfigValuePtr(MakeExperimentalComputationNode(config), location, exprPath);
        };
        info.isConfigRecord = false;
        return info;
    }
#endif

    // lookup table for "new" expression
    // This table lists all C++ types that can be instantiated from "new" expressions, and gives a constructor lambda and type flags.
    static map<wstring, ConfigurableRuntimeType> configurableRuntimeTypes =
    {
#define DefineRuntimeType(T) { L#T, MakeRuntimeTypeConstructor<T>() }
        // ComputationNodes
        DefineRuntimeType(ComputationNode),
        // other relevant classes
        DefineRuntimeType(NDLComputationNetwork),           // currently our fake
        // Functions
        DefineRuntimeType(StringFunction),
        DefineRuntimeType(NumericFunction),
        // Actions
        DefineRuntimeType(PrintAction),
        DefineRuntimeType(FailAction),
        DefineRuntimeType(AnotherAction),
        // glue to experimental integration
        //{ L"ExperimentalComputationNetwork", MakeExperimentalComputationNetworkConstructor() },
        //{ L"ComputationNode", MakeExperimentalComputationNodeConstructor() },
    };

    // -----------------------------------------------------------------------
    // name lookup
    // -----------------------------------------------------------------------

    static ConfigValuePtr Evaluate(ExpressionPtr e, ConfigRecordPtr scope, wstring exprPath, const wstring & exprId); // forward declare

    // look up a member by id in the search scope
    // If it is not found, it tries all lexically enclosing scopes inside out. This is handled by the ConfigRecord itself.
    static const ConfigValuePtr & ResolveIdentifier(const wstring & id, TextLocation idLocation, ConfigRecordPtr scope)
    {
        //if (!scope)                                           // no scope or went all the way up: not found
        //    UnknownIdentifier(id, idLocation);
        auto p = scope->Find(id);                               // look up the name
        if (!p)
            UnknownIdentifier(id, idLocation);
        //    return ResolveIdentifier(id, idLocation, scope->up);    // not found: try next higher scope
        // found it: resolve the value lazily (the value will hold a Thunk to compute its value upon first use)
        p->ResolveValue();          // if this is the first access, then the value will be a Thunk; this resolves it into the real value
        // now the value is available
        return *p;
    }

    // look up an identifier in an expression that is a ConfigRecord
    static ConfigValuePtr RecordLookup(ExpressionPtr recordExpr, const wstring & id, TextLocation idLocation, ConfigRecordPtr scope, const wstring & exprPath)
    {
        // Note on scope: The record itself (left of '.') must still be evaluated, and for that, we use the current scope;
        // that is, variables inside that expression--often a single variable referencing something in the current scope--
        // will be looked up there.
        // Now, the identifier on the other hand is looked up in the record and *its* scope (parent chain).
        let record = AsPtr<ConfigRecord>(Evaluate(recordExpr, scope, exprPath, L""), recordExpr, L"record");
        return ResolveIdentifier(id, idLocation, record/*resolve in scope of record; *not* the current scope*/);
    }

    // -----------------------------------------------------------------------
    // runtime-object creation
    // -----------------------------------------------------------------------

    // evaluate all elements in a dictionary expression and turn that into a ConfigRecord
    // which is meant to be passed to the constructor or Init() function of a runtime object
    static shared_ptr<ConfigRecord> ConfigRecordFromDictExpression(ExpressionPtr recordExpr, ConfigRecordPtr scope, const wstring & exprPath)
    {
        // evaluate the record expression itself
        // This will leave its members unevaluated since we do that on-demand
        // (order and what gets evaluated depends on what is used).
        let record = AsPtr<ConfigRecord>(Evaluate(recordExpr, scope, exprPath, L""), recordExpr, L"record");
        // resolve all entries, as they need to be passed to the C++ world which knows nothing about this
        return record;
    }

    // -----------------------------------------------------------------------
    // infix operators
    // -----------------------------------------------------------------------

    // entry for infix-operator lookup table
    typedef function<ConfigValuePtr(ExpressionPtr e, ConfigValuePtr leftVal, ConfigValuePtr rightVal, const wstring & exprPath)> InfixOp /*const*/;
    struct InfixOps
    {
        InfixOp NumbersOp;            // number OP number -> number
        InfixOp StringsOp;            // string OP string -> string
        InfixOp BoolOp;               // bool OP bool -> bool
        InfixOp ComputeNodeOp;        // ComputeNode OP ComputeNode -> ComputeNode
        InfixOp NumberComputeNodeOp;  // number OP ComputeNode -> ComputeNode, e.g. 3 * M
        InfixOp ComputeNodeNumberOp;  // ComputeNode OP Number -> ComputeNode, e.g. M * 3
        InfixOp DictOp;               // dict OP dict
        InfixOps(InfixOp NumbersOp, InfixOp StringsOp, InfixOp BoolOp, InfixOp ComputeNodeOp, InfixOp NumberComputeNodeOp, InfixOp ComputeNodeNumberOp, InfixOp DictOp)
            : NumbersOp(NumbersOp), StringsOp(StringsOp), BoolOp(BoolOp), ComputeNodeOp(ComputeNodeOp), NumberComputeNodeOp(NumberComputeNodeOp), ComputeNodeNumberOp(ComputeNodeNumberOp), DictOp(DictOp) { }
    };

    // functions that implement infix operations
    __declspec(noreturn)
    static void InvalidInfixOpTypes(ExpressionPtr e) { Fail(L"operator " + e->op + L" cannot be applied to these operands", e->location); }
    template<typename T>
    static ConfigValuePtr CompOp(ExpressionPtr e, const T & left, const T & right, const wstring & exprPath)
    {
        if (e->op == L"==")      return MakePrimitiveConfigValuePtr(left == right, e->location, exprPath);
        else if (e->op == L"!=") return MakePrimitiveConfigValuePtr(left != right, e->location, exprPath);
        else if (e->op == L"<")  return MakePrimitiveConfigValuePtr(left <  right, e->location, exprPath);
        else if (e->op == L">")  return MakePrimitiveConfigValuePtr(left >  right, e->location, exprPath);
        else if (e->op == L"<=") return MakePrimitiveConfigValuePtr(left <= right, e->location, exprPath);
        else if (e->op == L">=") return MakePrimitiveConfigValuePtr(left >= right, e->location, exprPath);
        else LogicError("unexpected infix op");
    }
    static ConfigValuePtr NumOp(ExpressionPtr e, ConfigValuePtr leftVal, ConfigValuePtr rightVal, const wstring & exprPath)
    {
        let left = leftVal.AsRef<Double>();
        let right = rightVal.AsRef<Double>();
        if (e->op == L"+")       return MakePrimitiveConfigValuePtr(left + right,      e->location, exprPath);
        else if (e->op == L"-")  return MakePrimitiveConfigValuePtr(left - right,      e->location, exprPath);
        else if (e->op == L"*")  return MakePrimitiveConfigValuePtr(left * right,      e->location, exprPath);
        else if (e->op == L"/")  return MakePrimitiveConfigValuePtr(left / right,      e->location, exprPath);
        else if (e->op == L"%")  return MakePrimitiveConfigValuePtr(fmod(left, right), e->location, exprPath);
        else if (e->op == L"**") return MakePrimitiveConfigValuePtr(pow(left, right),  e->location, exprPath);
        else return CompOp<double>(e, left, right, exprPath);
    };
    static ConfigValuePtr StrOp(ExpressionPtr e, ConfigValuePtr leftVal, ConfigValuePtr rightVal, const wstring & exprPath)
    {
        let left = leftVal.AsRef<String>();
        let right = rightVal.AsRef<String>();
        if (e->op == L"+")  return ConfigValuePtr(make_shared<String>(left + right), e->location, exprPath);
        else return CompOp<wstring>(e, left, right, exprPath);
    };
    static ConfigValuePtr BoolOp(ExpressionPtr e, ConfigValuePtr leftVal, ConfigValuePtr rightVal, const wstring & exprPath)
    {
        let left = leftVal.AsRef<Bool>();
        let right = rightVal.AsRef<Bool>();
        if (e->op == L"||")       return MakePrimitiveConfigValuePtr(left || right, e->location, exprPath);
        else if (e->op == L"&&")  return MakePrimitiveConfigValuePtr(left && right, e->location, exprPath);
        else if (e->op == L"^")   return MakePrimitiveConfigValuePtr(left ^  right, e->location, exprPath);
        else return CompOp<bool>(e, left, right, exprPath);
    };
    static ConfigValuePtr NodeOp(ExpressionPtr e, ConfigValuePtr leftVal, ConfigValuePtr rightVal, const wstring & exprPath)
    {
        if (rightVal.Is<Double>())          // ComputeNode * scalar
            swap(leftVal, rightVal);        // -> scalar * ComputeNode
        wstring classId;
        if (leftVal.Is<Double>())           // scalar * ComputeNode
        {
            if (e->op == L"*" || e->op == L"-(") classId = L"ScaleNode";    // "-(" is unary minus, which also calls this function with Double(-1) as leftVal
            else LogicError("unexpected infix op");
        }
        else                                // ComputeNode OP ComputeNode
        {
            if (e->op == L"+")       classId = L"PlusNode";
            else if (e->op == L"-")  classId = L"MinusNode";
            else if (e->op == L"*")  classId = L"TimesNode";
            else if (e->op == L".*") classId = L"DiagTimesNode";
            else LogicError("unexpected infix op");
        }
        // directly instantiate a ComputationNode for the magic operators * + and - that are automatically translated.
        // find creation lambda
        let newIter = configurableRuntimeTypes.find(L"ComputationNode");
        if (newIter == configurableRuntimeTypes.end())
            LogicError("unknown magic runtime-object class");
        // form the ConfigRecord
        ConfigRecord config(nullptr);
        // Note on scope: This config holds the arguments of the XXXNode runtime-object instantiations.
        // When they fetch their parameters, they should only look in this record, not in any parent scope (if they don't find what they are looking for, it's a bug in this routine here).
        // The values themselves are already in ConfigValuePtr form, so we won't need any scope lookups there either.
        config.Add(L"class", e->location, ConfigValuePtr(make_shared<String>(classId), e->location, exprPath));
        vector<ConfigValuePtr> inputs;
        inputs.push_back(leftVal);
        inputs.push_back(rightVal);
        config.Add(L"inputs", leftVal.GetLocation(), ConfigValuePtr(make_shared<ConfigArray>(0, move(inputs)), leftVal.GetLocation(), exprPath));
        // instantiate
        let value = newIter->second.construct(config, e->location, exprPath);
        let valueWithName = dynamic_cast<HasName*>(value.get());
        if (valueWithName)
            valueWithName->SetName(value.GetExpressionName());
        return value;
    };
    static ConfigValuePtr BadOp(ExpressionPtr e, ConfigValuePtr, ConfigValuePtr, const wstring &) { InvalidInfixOpTypes(e); };

    // lookup table for infix operators
    // This lists all infix operators with lambdas for evaluating them.
    static map<wstring, InfixOps> infixOps =
    {
        // NumbersOp StringsOp BoolOp ComputeNodeOp DictOp
        { L"*",  InfixOps(NumOp, BadOp, BadOp,  NodeOp, NodeOp, NodeOp, BadOp) },
        { L"/",  InfixOps(NumOp, BadOp, BadOp,  BadOp,  BadOp,  BadOp,  BadOp) },
        { L".*", InfixOps(BadOp, BadOp, BadOp,  NodeOp, BadOp,  BadOp,  BadOp) },
        { L"**", InfixOps(NumOp, BadOp, BadOp,  BadOp,  BadOp,  BadOp,  BadOp) },
        { L"%",  InfixOps(NumOp, BadOp, BadOp,  BadOp,  BadOp,  BadOp,  BadOp) },
        { L"+",  InfixOps(NumOp, StrOp, BadOp,  NodeOp, BadOp,  BadOp,  BadOp) },
        { L"-",  InfixOps(NumOp, BadOp, BadOp,  NodeOp, BadOp,  BadOp,  BadOp) },
        { L"==", InfixOps(NumOp, StrOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L"!=", InfixOps(NumOp, StrOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L"<",  InfixOps(NumOp, StrOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L">",  InfixOps(NumOp, StrOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L"<=", InfixOps(NumOp, StrOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L">=", InfixOps(NumOp, StrOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L"&&", InfixOps(BadOp, BadOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L"||", InfixOps(BadOp, BadOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) },
        { L"^",  InfixOps(BadOp, BadOp, BoolOp, BadOp,  BadOp,  BadOp,  BadOp) }
    };

    // -----------------------------------------------------------------------
    // thunked (delayed) evaluation
    // -----------------------------------------------------------------------

    // create a lambda that calls Evaluate() on an expr to get or realize its value
    static shared_ptr<Object> MakeEvaluateThunkPtr(ExpressionPtr expr, ConfigRecordPtr scope, const wstring & exprPath, const wstring & exprId)
    {
        function<ConfigValuePtr()> f = [expr, scope, exprPath, exprId]()   // lambda that computes this value of 'expr'
        {
            if (trace)
                TextLocation::PrintIssue(vector<TextLocation>(1, expr->location), L"", exprPath.c_str(), L"executing thunk");
            let value = Evaluate(expr, scope, exprPath, exprId);
            return value;   // this is a great place to set a breakpoint!
        };
        //return make_shared<ConfigValuePtr::Thunk>(f, expr->location);
        return ConfigValuePtr::MakeThunk(f, expr->location, exprPath);
    }

    // -----------------------------------------------------------------------
    // main evaluator function (highly recursive)
    // -----------------------------------------------------------------------

    // Evaluate()
    //  - input:  expression
    //  - output: ConfigValuePtr that holds the evaluated value of the expression
    //  - secondary inputs:
    //     - scope: parent ConfigRecord to pass on to nested ConfigRecords we create, for recursive name lookup
    //     - exprPath, exprId: for forming the expression path
    // On expression paths:
    //  - expression path encodes the path through the expression tree
    //  - this is meant to be able to give ComputationNodes a name for later lookup that behaves the same as looking up an object directly
    //  - not all nodes get their own path, in particular nodes with only one child, e.g. "-x", that would not be useful to address
    // Note that returned values may include complex value types like dictionaries (ConfigRecord) and functions (ConfigLambda).
    static ConfigValuePtr Evaluate(ExpressionPtr e, ConfigRecordPtr scope, wstring exprPath, const wstring & exprId)
    {
        try // catch clause for this will catch error, inject this tree node's TextLocation, and rethrow
        {
            // expression names
            // Merge exprPath and exprId into one unless one is empty
            if (!exprPath.empty() && !exprId.empty())
                exprPath.append(exprPathSeparator);
            exprPath.append(exprId);
            // tracing
            if (trace)
                TextLocation::PrintIssue(vector<TextLocation>(1, e->location), L"", L"", L"trace");
            // --- literals
            if (e->op == L"d")       return MakePrimitiveConfigValuePtr(e->d, e->location, exprPath);         // === double literal
            else if (e->op == L"s")  return ConfigValuePtr(make_shared<String>(e->s), e->location, exprPath); // === string literal
            else if (e->op == L"b")  return MakePrimitiveConfigValuePtr(e->b, e->location, exprPath);         // === bool literal
            else if (e->op == L"new")                                                               // === 'new' expression: instantiate C++ runtime object right here
            {
                // find the constructor lambda
                let newIter = configurableRuntimeTypes.find(e->id);
                if (newIter == configurableRuntimeTypes.end())
                    Fail(L"unknown runtime type " + e->id, e->location);
                // form the config record
                let dictExpr = e->args[0];
                let argsExprPath = newIter->second.isConfigRecord ? L"" : exprPath;   // reset expr-name path if object exposes a dictionary
                let value = newIter->second.construct(*ConfigRecordFromDictExpression(dictExpr, scope, argsExprPath), e->location, exprPath); // this constructs it
                // if object has a name, we set it
                let valueWithName = dynamic_cast<HasName*>(value.get());
                if (valueWithName)
                    valueWithName->SetName(value.GetExpressionName());
                return value;   // we return the created but not initialized object as the value, so others can reference it
            }
            else if (e->op == L"if")                                                    // === conditional expression
            {
                let condition = ToBoolean(Evaluate(e->args[0], scope, exprPath, L"if"), e->args[0]);
                if (condition)
                    return Evaluate(e->args[1], scope, exprPath, L"");      // pass exprName through 'if' since only of the two exists
                else
                    return Evaluate(e->args[2], scope, exprPath, L"");
            }
            // --- functions
            else if (e->op == L"=>")                                                    // === lambda (all macros are stored as lambdas)
            {
                // on scope: The lambda expression remembers the lexical scope of the '=>'; this is how it captures its context.
                let argListExpr = e->args[0];           // [0] = argument list ("()" expression of identifiers, possibly optional args)
                if (argListExpr->op != L"()") LogicError("parameter list expected");
                let fnExpr = e->args[1];                // [1] = expression of the function itself
                let f = [argListExpr, fnExpr, scope, exprPath](const vector<ConfigValuePtr> & args, const ConfigLambda::NamedParams & namedArgs, const wstring & callerExprPath) -> ConfigValuePtr
                {
                    // TODO: document namedArgs--does it have a parent scope? Or is it just a dictionary? Should we just use a shared_ptr<map,ConfigValuPtr>> instead for clarity?
                    // on exprName
                    //  - 'callerExprPath' is the name to which the result of the fn evaluation will be assigned
                    //  - 'exprPath' (outside) is the name of the macro we are defining this lambda under
                    let & argList = argListExpr->args;
                    if (args.size() != argList.size()) LogicError("function application with mismatching number of arguments");
                    // To execute a function body with passed arguments, we
                    //  - create a new scope that contains all positional and named args
                    //  - then evaluate the expression with that scope
                    //  - parent scope for this is the scope of the function definition (captured context)
                    //    Note that the 'scope' variable in here (we are in a lambda) is the scope of the '=>' expression, that is, the macro definition.
                    // create a ConfigRecord with param names from 'argList' and values from 'args'
                    let argScope = make_shared<ConfigRecord>(scope); // look up in params first; then proceed upwards in lexical scope of '=>' (captured context)
                    //let thisScope = MakeScope(argScope, scope);   
                    // create an entry for every argument value
                    // Note that these values should normally be thunks since we only want to evaluate what's used.
                    for (size_t i = 0; i < args.size(); i++)    // positional arguments
                    {
                        let argName = argList[i];       // parameter name
                        if (argName->op != L"id") LogicError("function parameter list must consist of identifiers");
                        let & argVal = args[i];         // value of the parameter
                        argScope->Add(argName->id, argName->location, argVal);
                        // note: these are expressions for the parameter values; so they must be evaluated in the current scope
                    }
                    // also named arguments
                    for (let namedArg : namedArgs)
                    {
                        let id = namedArg.first;
                        let & argVal = namedArg.second;
                        argScope->Add(id, argVal.GetLocation(), argVal);
                    }
                    // get the macro name for the exprPath
                    wstring macroId = exprPath;
                    let pos = macroId.find(exprPathSeparator);
                    if (pos != wstring::npos)
                        macroId.erase(0, pos + 1);
                    // now evaluate the function
                    return Evaluate(fnExpr, argScope, callerExprPath, L"[" + macroId + L"]");  // bring args into scope; keep lex scope of '=>' as upwards chain
                };
                // positional args
                vector<wstring> paramNames;
                let & argList = argListExpr->args;
                for (let arg : argList)
                {
                    if (arg->op != L"id") LogicError("function parameter list must consist of identifiers");
                    paramNames.push_back(arg->id);
                }
                // named args
                // The nammedArgs in the definition lists optional arguments with their default values
                ConfigLambda::NamedParams namedParams;
                for (let namedArg : argListExpr->namedArgs)
                {
                    let id = namedArg.first;
                    let location = namedArg.second.first;   // location of identifier
                    let expr = namedArg.second.second;      // expression to evaluate to get default value
                    namedParams[id] = ConfigValuePtr(MakeEvaluateThunkPtr(expr, scope/*evaluate default value in context of definition*/, exprPath, id), expr->location, exprPath/*TODO??*/);
                    //namedParams->Add(id, location/*loc of id*/, ConfigValuePtr(MakeEvaluateThunkPtr(expr, scope/*evaluate default value in context of definition*/, exprPath, id), expr->location, exprPath/*TODO??*/));
                    // the thunk is called if the default value is ever used
                }
                return ConfigValuePtr(make_shared<ConfigLambda>(move(paramNames), move(namedParams), f), e->location, exprPath);
            }
            else if (e->op == L"(")                                         // === apply a function to its arguments
            {
                let lambdaExpr = e->args[0];            // [0] = function
                let argsExpr = e->args[1];              // [1] = arguments passed to the function ("()" expression of expressions)
                let lambda = AsPtr<ConfigLambda>(Evaluate(lambdaExpr, scope, exprPath, L"_lambda"), lambdaExpr, L"function");
                if (argsExpr->op != L"()") LogicError("argument list expected");
                // put all args into a vector of values
                // Like in an [] expression, we do not evaluate at this point, but pass in a lambda to compute on-demand.
                let args = argsExpr->args;
                if (args.size() != lambda->GetNumParams())
                    Fail(L"function parameter list must consist of identifiers", argsExpr->location);
                vector<ConfigValuePtr> argVals(args.size());
                for (size_t i = 0; i < args.size(); i++)    // positional arguments
                {
                    let argValExpr = args[i];               // expression to evaluate arg [i]
                    let argName = lambda->GetParamNames()[i];
#if 1
                    argVals[i] = Evaluate(argValExpr, scope, exprPath, L"(" + argName + L")");  // evaluate right here
                    // We evaluate all macros at time of macro invocation, not at time of first use inside the macro.
                    // This is to make the ConfigValuePtr single-ownership-while-thunked problem easier.
                    // Revisit this if this ever causes a problem.
#else
                    argVals[i] = ConfigValuePtr(MakeEvaluateThunkPtr(argValExpr, scope, exprPath, L"(" + argName + L")"), argValExpr->location, exprPath/*TODO??*/);  // make it a thunked value
#endif
                    /*this wstrprintf should be gone, this is now the exprName*/
                    // Note on scope: macro arguments form a scope (ConfigRecord), the expression for an arg does not have access to that scope.
                    // E.g. F(A,B) is used as F(13,A) then that A must come from outside, it is not the function argument.
                    // This is a little inconsistent with real records, e.g. [ A = 13 ; B = A ] where this A now does refer to this record.
                    // However, it is still the expected behavior, because in a real record, the user sees all the other names, while when
                    // passing args to a function, he does not; and also the parameter names can depend on the specific lambda being used.
                }
                // named args are put into a ConfigRecord
                // We could check whether the named ars are actually accepted by the lambda, but we leave that to Apply() so that the check also happens for lambda calls from CNTK C++ code.
                let namedArgs = argsExpr->namedArgs;
                ConfigLambda::NamedParams namedArgVals;
                // TODO: no scope here? ^^ Where does the scope come in? Maybe not needed since all values are already resolved? Document this!
                for (let namedArg : namedArgs)
                {
                    let id = namedArg.first;                // id of passed in named argument
                    let location = namedArg.second.first;   // location of expression
                    let expr = namedArg.second.second;      // expression of named argument
#if 1
                    namedArgVals[id] = Evaluate(expr, scope, exprPath, id);
#else
                    namedArgVals[id] = ConfigValuePtr(MakeEvaluateThunkPtr(expr, scope, exprPath, id), expr->location, exprPath/*TODO??*/);
                    //namedArgVals->Add(id, location/*loc of id*/, ConfigValuePtr(MakeEvaluateThunkPtr(expr, scope, exprPath, id), expr->location, exprPath/*TODO??*/));
                    // the thunk is evaluated when/if the passed actual value is ever used the first time
#endif
                    // Note on scope: same as above.
                    // E.g. when a function declared as F(A=0,B=0) is called as F(A=13,B=A), then A in B=A is not A=13, but anything from above.
                    // For named args, it is far less clear whether users would expect this. We still do it for consistency with positional args, which are far more common.
                }
                // call the function!
                return lambda->Apply(argVals, namedArgVals, exprPath);
            }
            // --- variable access
            else if (e->op == L"[]")                                                // === record (-> ConfigRecord)
            {
                let newScope = make_shared<ConfigRecord>(scope);      // new scope: inside this record, all symbols from above are also visible
                // create an entry for every dictionary entry.
                //let thisScope = MakeScope(record, scope);         // lexical scope includes this dictionary itself, so we can access forward references
                // We do not evaluate the members at this point.
                // Instead, as the value, we keep the ExpressionPtr itself wrapped in a lambda that evaluates that ExpressionPtr to a ConfigValuePtr when called.
                // Members are evaluated on demand when they are used.
                for (let & entry : e->namedArgs)
                {
                    let id = entry.first;
                    let expr = entry.second.second;             // expression to compute the entry
                    newScope->Add(id, entry.second.first/*loc of id*/, ConfigValuePtr(MakeEvaluateThunkPtr(expr, newScope/*scope*/, exprPath, id), expr->location, exprPath/*TODO??*/));
                    // Note on scope: record assignments are like a "let rec" in F#/OCAML. That is, all record members are visible to all
                    // expressions that initialize the record members. E.g. [ A = 13 ; B = A ] assigns B as 13, not to a potentially outer A.
                    // (To explicitly access an outer A, use the slightly ugly syntax ...A)
                }
                // BUGBUG: wrong text location passed in. Should be the one of the identifier, not the RHS. NamedArgs store no location for their identifier.
                return ConfigValuePtr(newScope, e->location, exprPath);
            }
            else if (e->op == L"id") return ResolveIdentifier(e->id, e->location, scope);   // === variable/macro access within current scope
            else if (e->op == L".")                                                         // === variable/macro access in given ConfigRecord element
            {
                let recordExpr = e->args[0];
                return RecordLookup(recordExpr, e->id, e->location, scope/*for evaluating recordExpr*/, exprPath);
            }
            // --- arrays
            else if (e->op == L":")                                                         // === array expression (-> ConfigArray)
            {
                // this returns a flattened list of all members as a ConfigArray type
                let arr = make_shared<ConfigArray>();       // note: we could speed this up by keeping the left arg and appending to it
                for (size_t i = 0; i < e->args.size(); i++) // concatenate the two args
                {
                    let expr = e->args[i];
                    let item = Evaluate(expr, scope, exprPath, wstrprintf(L"_vecelem%d", i));           // result can be an item or a vector
                    if (item.Is<ConfigArray>())
                        arr->Append(item.AsRef<ConfigArray>());     // append all elements (this flattens it)
                    else
                        arr->Append(item);
                }
                return ConfigValuePtr(arr, e->location, exprPath);  // location will be that of the first ':', not sure if that is best way
            }
            else if (e->op == L"array")                                                     // === array constructor from lambda function
            {
                let firstIndexExpr = e->args[0];    // first index
                let lastIndexExpr  = e->args[1];    // last index
                let initLambdaExpr = e->args[2];    // lambda to initialize the values
                let firstIndex = ToInt(Evaluate(firstIndexExpr, scope, exprPath, L"array_first"), firstIndexExpr);
                let lastIndex  = ToInt(Evaluate(lastIndexExpr,  scope, exprPath, L"array_last"),  lastIndexExpr);
                let lambda = AsPtr<ConfigLambda>(Evaluate(initLambdaExpr, scope, exprPath, L"_initializer"), initLambdaExpr, L"function");
                if (lambda->GetNumParams() != 1)
                    Fail(L"'array' requires an initializer function with one argument (the index)", initLambdaExpr->location);
                // At this point, we must know the dimensions and the initializer lambda, but we don't need to know all array elements.
                // Resolving array members on demand allows recursive access to the array variable, e.g. h[t] <- f(h[t-1]).
                // create a vector of Thunks to initialize each value
                vector<ConfigValuePtr> elementThunks;
                for (int index = firstIndex; index <= lastIndex; index++)
                {
                    let indexValue = MakePrimitiveConfigValuePtr((double)index, e->location, exprPath/*never needed*/);           // index as a ConfigValuePtr
                    let elemExprPath = exprPath.empty() ? L"" : wstrprintf(L"%ls[%d]", exprPath.c_str(), index);    // expression name shows index lookup
                    let initExprPath = exprPath.empty() ? L"" : wstrprintf(L"_lambda");    // expression name shows initializer with arg
                    // create an expression
                    function<ConfigValuePtr()> f = [indexValue, initLambdaExpr, scope, elemExprPath, initExprPath]()   // lambda that computes this value of 'expr'
                    {
                        if (trace)
                            TextLocation::PrintIssue(vector<TextLocation>(1, initLambdaExpr->location), L"", wstrprintf(L"index %d", (int)indexValue).c_str(), L"executing array initializer thunk");
                        // apply initLambdaExpr to indexValue and return the resulting value
                        let initLambda = AsPtr<ConfigLambda>(Evaluate(initLambdaExpr, scope, initExprPath, L""), initLambdaExpr, L"function");  // get the function itself (most of the time just a simple name)
                        vector<ConfigValuePtr> argVals(1, indexValue);      // create an arg list with indexValue as the one arg
                        //NamedArgs namedArgs = make_shared<ConfigRecord>(nullptr); // no named args in initializer lambdas TODO: change to shared_ptr<map<>>
                        // TODO: where does the current scope come in? Aren't we looking up in namedArgs directly?
                        let value = initLambda->Apply(argVals, ConfigLambda::NamedParams(), elemExprPath);
                        return value;   // this is a great place to set a breakpoint!
                    };
                    elementThunks.push_back(ConfigValuePtr::MakeThunk(f, initLambdaExpr->location, elemExprPath/*TODO??*/));
                }
                auto arr = make_shared<ConfigArray>(firstIndex, move(elementThunks));
                return ConfigValuePtr(arr, e->location, exprPath);
            }
            else if (e->op == L"[")                                         // === access array element by index
            {
                let arrValue = Evaluate(e->args[0], scope, exprPath, L"_vector");
                let indexExpr = e->args[1];
                let arr = AsPtr<ConfigArray>(arrValue, indexExpr, L"array");
                let index = ToInt(Evaluate(indexExpr, scope, exprPath, L"_index"), indexExpr);
                return arr->At(index, indexExpr->location); // note: the array element may be as of now unresolved; this resolved it
            }
            // --- unary operators '+' '-' and '!'
            else if (e->op == L"+(" || e->op == L"-(")                      // === unary operators + and -
            {
                let argExpr = e->args[0];
                let argValPtr = Evaluate(argExpr, scope, exprPath, e->op == L"+(" ? L"" : L"_negate");
                // note on exprPath: since - has only one argument, we do not include it in the expessionPath
                if (argValPtr.Is<Double>())
                    if (e->op == L"+(") return argValPtr;
                    else return MakePrimitiveConfigValuePtr(-(double)argValPtr, e->location, exprPath);
                else if (argValPtr.Is<ComputationNode>())   // -ComputationNode becomes ScaleNode(-1,arg)
                    if (e->op == L"+(") return argValPtr;
                    else return NodeOp(e, MakePrimitiveConfigValuePtr(-1.0, e->location, exprPath), argValPtr, exprPath);
                else
                    Fail(L"operator '" + e->op.substr(0, 1) + L"' cannot be applied to this operand (which has type " + msra::strfun::utf16(argValPtr.TypeName()) + L")", e->location);
            }
            else if (e->op == L"!(")                                        // === unary operator !
            {
                let arg = ToBoolean(Evaluate(e->args[0], scope, exprPath, L"_not"), e->args[0]);
                return MakePrimitiveConfigValuePtr(!arg, e->location, exprPath);
            }
            // --- regular infix operators such as '+' and '=='
            else
            {
                let opIter = infixOps.find(e->op);
                if (opIter == infixOps.end())
                    LogicError("e->op " + utf8(e->op) + " not implemented");
                let & functions = opIter->second;
                let leftArg = e->args[0];
                let rightArg = e->args[1];
                let leftValPtr  = Evaluate(leftArg,  scope, exprPath, L"[" + e->op + L"](left)");
                let rightValPtr = Evaluate(rightArg, scope, exprPath, L"[" + e->op + L"](right)");
                if (leftValPtr.Is<Double>() && rightValPtr.Is<Double>())
                    return functions.NumbersOp(e, leftValPtr, rightValPtr, exprPath);
                else if (leftValPtr.Is<String>() && rightValPtr.Is<String>())
                    return functions.StringsOp(e, leftValPtr, rightValPtr, exprPath);
                else if (leftValPtr.Is<Bool>() && rightValPtr.Is<Bool>())
                    return functions.BoolOp(e, leftValPtr, rightValPtr, exprPath);
                // ComputationNode is "magic" in that we map *, +, and - to know classes of fixed names.
                else if (leftValPtr.Is<ComputationNode>() && rightValPtr.Is<ComputationNode>())
                    return functions.ComputeNodeOp(e, leftValPtr, rightValPtr, exprPath);
                else if (leftValPtr.Is<ComputationNode>() && rightValPtr.Is<Double>())
                    return functions.ComputeNodeNumberOp(e, leftValPtr, rightValPtr, exprPath);
                else if (leftValPtr.Is<Double>() && rightValPtr.Is<ComputationNode>())
                    return functions.NumberComputeNodeOp(e, leftValPtr, rightValPtr, exprPath);
                // TODO: DictOp  --maybe not; maybedo this in ModelMerger class instead
                else
                    InvalidInfixOpTypes(e);
            }
            //LogicError("should not get here");
        }
        catch (ConfigError & err)
        {
            // in case of an error, we keep track of all parent locations in the parse as well, to make it easier for the user to spot the error
            err.AddLocation(e->location);
            throw;
        }
    }

    static ConfigValuePtr EvaluateParse(ExpressionPtr e)
    {
        return Evaluate(e, nullptr/*top scope*/, L"", L"$");
    }

    // -----------------------------------------------------------------------
    // external entry points
    // -----------------------------------------------------------------------

    // top-level entry
    // A config sequence X=A;Y=B;do=(A,B) is really parsed as [X=A;Y=B].do. That's the tree we get. I.e. we try to compute the 'do' member.
    void Do(ExpressionPtr e)
    {
        RecordLookup(e, L"do", e->location, nullptr, L"$");  // we evaluate the member 'do'
    }

    shared_ptr<Object> EvaluateField(ExpressionPtr e, const wstring & id)
    {
        //let record = AsPtr<ConfigRecord>(Evaluate(recordExpr, scope, exprPath, L""), recordExpr, L"record");
        //return ResolveIdentifier(id, idLocation, MakeScope(record, nullptr/*no up scope*/));
        return RecordLookup(e, id, e->location, nullptr/*scope for evaluating 'e'*/, L"$");  // we evaluate the member 'do'
    }

    ConfigValuePtr Evaluate(ExpressionPtr e)
    {
        return /*Evaluator().*/EvaluateParse(e);
    }

}}}}     // namespaces