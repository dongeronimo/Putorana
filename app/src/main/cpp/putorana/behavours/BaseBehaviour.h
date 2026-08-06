#ifndef ARRECONSTRUCTOR_BASEBEHAVIOUR_H
#define ARRECONSTRUCTOR_BASEBEHAVIOUR_H
#include <functional>
#include <optional>
namespace putorana::graphics {
    class Node;
}
namespace putorana::behaviours {
    //Base class of all behaviours, strongly inspired by Unity's MonoBehaviours.
    //Behaviours exist to define what a node do. A node can have many behaviours,
    //order of execution in a given node is not guaranteed, but in the world order
    //of execution follows the the tree structure.
    //Lifecycle:
    //Start is called when the behaviour is executed for the first time. It'll  be
    //called just before it's first update.
    //Update is called every frame.
    //LateUpdate is called, if present, in a second update pass.
    //Dispose should be called before destroying the behaviour.
    class BaseBehaviour {
    private:
        //Start should run only once. This flags controls that.
        bool _alreadyStarted = false;
    protected:
        //ptr to the owning node. BaseBehaviour must not try to delete this.
        //Protected and not private: the node is how a behaviour reaches anything
        //at all, so a subclass that cannot see it cannot do its job. It is not
        //public because it is the subclass's own link, not something callers set.
        putorana::graphics::Node* owner = nullptr;
        std::optional<std::function<void(float, BaseBehaviour*)>> lateUpdate;
    public:
        // The behavour must know it's owning node.
        BaseBehaviour(putorana::graphics::Node* node);
        // Will be false until Start runs.
        bool alreadyStarted()const {return _alreadyStarted;}
        // Do we have late update function?
        bool hasLateUpdate()const { return lateUpdate.has_value(); }
        void runLateUpdate(float dt);
        // You need to override this if you want to do anything when the behaviour
        // starts. Remember to call Super::Start() because this function here is what
        //flips alreadyStarted to true;
        virtual void start();
        virtual void update(float dt) = 0;
        //Don't really need to implement it if you have nothing to dispose.
        virtual void dispose(){};
    };
}

#endif //ARRECONSTRUCTOR_BASEBEHAVIOUR_H
